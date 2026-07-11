module;
#include <bit>

#include "jit_typed_mutation_stats.h"

module aura.compiler.lowering;
import std;
import aura.core.ast;
import aura.compiler.lowering_linear_types;
import aura.compiler.value;

namespace aura::compiler {

using namespace aura::ir;
using namespace aura::ast;

// Issue #684: thread-local snapshot of the last dual-emit lower.
static thread_local LowerSoAEmitSnapshot g_last_soa_snapshot;

// Issue #657: compiler-core incremental observability hooks.
static thread_local LoweringObservabilityHooks g_lowering_hooks{};

void set_lowering_observability_hooks(LoweringObservabilityHooks hooks) noexcept {
    g_lowering_hooks = hooks;
}

void clear_lowering_observability_hooks() noexcept {
    g_lowering_hooks = {};
}

void LoweringState::emit_with_metadata(aura::ir::IROpcode op, std::uint32_t tid,
                                       std::uint8_t linear_state, std::uint32_t adt_variant,
                                       std::uint32_t narrow_evidence, std::uint32_t op0,
                                       std::uint32_t op1, std::uint32_t op2, std::uint32_t op3) {
    emit_with_type(op, tid, op0, op1, op2, op3);
    if (cur_func && cur_block < cur_func->blocks.size()) {
        auto& last = cur_func->blocks[cur_block].instructions.back();
        last.linear_ownership_state = linear_state;
        last.adt_variant_id = adt_variant;
        last.narrow_evidence = narrow_evidence;
        if (linear_state != 0 && g_lowering_hooks.on_linear_metadata_flow)
            g_lowering_hooks.on_linear_metadata_flow();
        if (dual_emit_soa && cur_func_v2_idx < module_v2.functions.size()) {
            std::uint8_t coercion_tag =
                op == aura::ir::IROpcode::CastOp ? static_cast<std::uint8_t>(op2) : 0;
            module_v2.patch_last_instruction_metadata(cur_func_v2_idx, linear_state, adt_variant,
                                                      narrow_evidence, coercion_tag);
            if (narrow_evidence != 0 || tid != 0) {
                ++soa_type_metadata_stamped;
                jit_typed_mutation::record_type_propagation_stamp();
            }
        }
    }
}

const LowerSoAEmitSnapshot* lower_last_soa_snapshot() noexcept {
    return &g_last_soa_snapshot;
}

// Map TypeId to CastOp type_tag (used by IR interpreter)
// INT→0, STRING→1, BOOL→2, FLOAT→4, DYNAMIC→3
static std::uint32_t type_tag_for_coercion(aura::core::TypeId tid,
                                           const aura::core::TypeRegistry* type_reg) {
    if (!type_reg)
        return 3;
    auto tag = type_reg->tag_of(tid);
    switch (tag) {
        case aura::core::TypeTag::INT:
            return 0;
        case aura::core::TypeTag::STRING:
            return 1;
        case aura::core::TypeTag::BOOL:
            return 2;
        case aura::core::TypeTag::FLOAT:
            return 4;
        default:
            return 3; // Dynamic / pass-through
    }
}

// ── Internal: native FlatAST lowering helpers ──────────────────
// Lower a FlatAST node to IR instructions. Returns the result slot.
// Reads FlatAST directly without reconstructing to Expr*.
// For Lambda nodes, reconstructs just the lambda body (needed by closure table).
// cache: optional map from variable name → bundle of IRFunctions for cached defines.

// Remap func ids inside a cached IRFunction when inlining into a new module.
// Each MakeClosure instruction's func_id is offset by base_fid, because the new
// module has functions added after its own __top__ function at a different base.
static void remap_func_ids(aura::ir::IRFunction& func, std::uint32_t base_fid) {
    // Issue #1089 / #660: cache-bundle functions have original fids
    // starting at 1 (entry at 0 excluded). When loaded into a new
    // module they land at base_fid + (original_fid - 1).
    // new_ref = original_ref + base_fid - 1.
    // When base_fid==0 this is original_ref-1 (empty module case) —
    // still required. Guard only on original_ref>=1 to avoid uint32 wrap.
    for (auto& block : func.blocks) {
        for (auto& inst : block.instructions) {
            if (inst.opcode == aura::ir::IROpcode::MakeClosure) {
                const auto orig = inst.operands[1];
                if (orig >= 1)
                    inst.operands[1] = orig + base_fid - 1;
            }
        }
    }
}

static std::uint32_t lower_flat_expr(
    LoweringState& state, const FlatAST& flat, StringPool& pool, NodeId id,
    const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>* cache = nullptr,
    std::vector<std::string>* cache_hits = nullptr) {
    // Track current flat/pool for closure bridge data
    state.current_flat = &flat;
    state.current_pool = &pool;
    LoweringState::SourceScope scope(state, id); // RAII: protects type_id propagation
    // Issue #507: debug contract on valid NodeId range (observe mode
    // uses early exit below instead of aborting on stale ids).
    contract_assert(id == NULL_NODE || id < flat.size());
    // Early exit for invalid ids (backup for contract-observe mode)
    if (id == NULL_NODE || id >= flat.size()) {
        auto slot = state.alloc_local();
        state.emit(IROpcode::ConstI64, slot, 0, 0);
        return slot;
    }
    auto v = flat.get(id);

    switch (v.tag) {
        case NodeTag::LiteralFloat: {
            auto slot = state.alloc_local();
            std::uint64_t bits = std::bit_cast<std::uint64_t>(v.float_value);
            state.emit(IROpcode::ConstF64, slot, static_cast<std::uint32_t>(bits & 0xFFFFFFFF),
                       static_cast<std::uint32_t>(bits >> 32));
            return slot;
        }
        case NodeTag::LiteralInt: {
            auto slot = state.alloc_local();
            auto marker = flat.marker(id);
            if (marker == aura::ast::SyntaxMarker::BoolLiteral) {
                // Bool literals (#t/#f) should produce make_bool, not make_int
                state.emit(IROpcode::ConstBool, slot, v.int_value != 0 ? 1 : 0);
            } else {
                auto val = static_cast<std::uint64_t>(v.int_value);
                state.emit(IROpcode::ConstI64, slot, static_cast<std::uint32_t>(val),
                           static_cast<std::uint32_t>(val >> 32));
            }
            return slot;
        }
        case NodeTag::LiteralString: {
            auto s = pool.resolve(v.sym_id);
            auto si = state.module.add_string(std::string(s));
            auto slot = state.alloc_local();
            state.emit(IROpcode::ConstString, slot, si, 0);
            return slot;
        }
        case NodeTag::Variable: {
            auto name = pool.resolve(v.sym_id);
            // Look up in scope chain
            for (auto it = state.scopes.rbegin(); it != state.scopes.rend(); ++it) {
                auto found = it->find(std::string(name));
                if (found != it->end()) {
                    auto& binding = found->second;
                    auto slot = state.alloc_local();
                    switch (binding.kind) {
                        case BindingKind::Local:
                            state.emit(IROpcode::Local, slot, binding.slot);
                            break;
                        case BindingKind::Captured:
                            state.emit(IROpcode::Local, slot, binding.slot);
                            break;
                        case BindingKind::Cell:
                            state.emit(IROpcode::CellGet, slot, binding.slot);
                            break;
                    }
                    return slot;
                }
            }
            // Free variable from outer lambda
            auto fv = state.free_var_map.find(std::string(name));
            if (fv != state.free_var_map.end()) {
                auto slot = state.alloc_local();
                state.emit(IROpcode::Local, slot, fv->second);
                return slot;
            }
            // Issue #63723: top-level value defines bound via IR take
            // precedence over the function-cache path. Without this,
            // a value-define like `(define a 1)` is cached as a
            // closure (NewCell + CellSet + return 1), and the
            // cache-hit path below would emit MakeClosure — which
            // when used in arithmetic, coerces to 0 instead of the
            // actual value 1. The value_cells map (populated by
            // bind_value_define_via_ir in populate_ir_cache_v2_from_workspace)
            // records the cell_id holding the literal value; emit
            // TopCellLoad to fetch it directly. The function cache
            // is still consulted (for function-define bindings which
            // DO want a closure); this check just runs first.
            if (state.value_cells) {
                auto vc_it = state.value_cells->find(std::string(name));
                if (vc_it != state.value_cells->end()) {
                    auto slot = state.alloc_local();
                    state.emit(IROpcode::TopCellLoad, slot,
                               static_cast<std::uint32_t>(vc_it->second));
                    return slot;
                }
            }
            // Check if this variable names a cached define function
            if (cache) {
                auto cache_it = cache->find(std::string(name));
                if (cache_it != cache->end()) {
                    if (cache_hits) {
                        cache_hits->push_back(std::string(name));
                    }
                    // Record the starting func id (offset) before adding bundle functions
                    auto base_fid = static_cast<std::uint32_t>(state.module.functions.size());
                    // Add all bundle functions to module, remapping func ids
                    std::uint32_t lambda_fid = 0;
                    for (std::size_t ci = 0; ci < cache_it->second.size(); ++ci) {
                        auto& func = cache_it->second[ci];
                        auto copy = func;
                        remap_func_ids(copy, base_fid);
                        // Remap ConstString pool indices using cached string pool
                        if (state.cache_strings) {
                            auto str_it = state.cache_strings->find(std::string(name));
                            if (str_it != state.cache_strings->end() && !str_it->second.empty()) {
                                auto& str_pool = str_it->second;
                                for (auto& blk : copy.blocks)
                                    for (auto& inst : blk.instructions)
                                        if (inst.opcode == IROpcode::ConstString &&
                                            inst.operands[1] < str_pool.size())
                                            inst.operands[1] =
                                                state.module.add_string(str_pool[inst.operands[1]]);
                            }
                        }
                        // Issue #660: find the user-defined lambda by NAME.
                        // The cache key (the 'name' variable) is what the user
                        // is looking up. The user-defined lambda has a name of
                        // the form "<name>#0" (set by cache_define). Match by name.
                        bool is_user_lambda = copy.name == std::string(name) + std::string("#0");
                        auto new_fid = state.module.add_function(std::move(copy));
                        if (is_user_lambda) {
                            lambda_fid = new_fid;
                        }
                        // Copy bridge data from cache bridge if available
                        if (state.cache_bridge) {
                            auto bridge_it = state.cache_bridge->find(std::string(name));
                            if (bridge_it != state.cache_bridge->end() &&
                                ci < bridge_it->second.size()) {
                                // Issue #741: limit bridge shared_ptr copy to
                                // func indices in the impact_scope set (partial
                                // re-lower). Non-impacted lambdas get epoch-only
                                // refresh so stale flat/pool are not retained.
                                const bool in_impact_scope =
                                    !g_lowering_hooks.impact_func_indices ||
                                    g_lowering_hooks.impact_func_indices->count(ci) > 0;
                                // Issue #224 Cycle 2: shared_ptr<>-based
                                // bridge. The cache bridge holds a
                                // shared_ptr; we copy it (refcount
                                // incremented) so the new bridge keeps
                                // the FlatAST alive independently.
                                // Issue #657: sync bridge_epoch on cache-hit copy so
                                // recursive-define closures see current mutation epoch.
                                std::uint64_t bridge_epoch = bridge_it->second[ci].bridge_epoch;
                                if (g_lowering_hooks.bridge_epoch_capture != 0)
                                    bridge_epoch = g_lowering_hooks.bridge_epoch_capture;
                                if (in_impact_scope) {
                                    state.module.set_closure_bridge_ptr(
                                        new_fid, bridge_it->second[ci].flat,
                                        bridge_it->second[ci].pool, bridge_it->second[ci].body_id,
                                        bridge_epoch);
                                    if (g_lowering_hooks.on_quote_lambda_bridge_copy)
                                        g_lowering_hooks.on_quote_lambda_bridge_copy();
                                } else {
                                    state.module.set_closure_bridge_ptr(
                                        new_fid, {}, {}, aura::ast::NULL_NODE, bridge_epoch);
                                }
                                if (g_lowering_hooks.on_bridge_epoch_sync)
                                    g_lowering_hooks.on_bridge_epoch_sync();
                                if (g_lowering_hooks.on_env_version_resync)
                                    g_lowering_hooks.on_env_version_resync();
                                // Copy body_source for bridge fallback re-parse
                                if (in_impact_scope && !bridge_it->second[ci].body_source.empty() &&
                                    new_fid < state.module.closure_bridge.size()) {
                                    state.module.closure_bridge[new_fid].body_source =
                                        bridge_it->second[ci].body_source;
                                }
                            }
                        }
                    }
                    auto closure_slot = state.alloc_local();
                    state.emit(IROpcode::MakeClosure, closure_slot, lambda_fid, 0);
                    return closure_slot;
                }
            }
            // Check if this variable names a known primitive
            if (state.primitives) {
                auto slot_idx = state.primitives->slot_for_name(std::string(name));
                if (slot_idx < state.primitives->slot_count()) {
                    auto slot = state.alloc_local();
                    state.emit(IROpcode::Primitive, slot, static_cast<std::uint32_t>(slot_idx));
                    return slot;
                }
            }
            // Self-reference: function being defined calls itself by name.
            // When the lambda has been pre-allocated in the module, emit
            // MakeClosure with the correct func_id instead of ConstI64 0.
            if (state.self_func_id != 0 && std::string(name) == state.self_name) {
                auto slot = state.alloc_local();
                state.emit(IROpcode::MakeClosure, slot, state.self_func_id, 0);
                return slot;
            }
            // (value_cells check was moved earlier — see Issue #63723.)
            auto slot = state.alloc_local();
            state.emit(IROpcode::ConstI64, slot, 0, 0);
            return slot;
        }
        case NodeTag::Call: {
            auto callee_v = flat.get(v.child(0));

            // Check if callee is a known primitive (+, -, *, /, =, <, >, <=, >=)
            // and inline as direct IR opcode to avoid Call overhead.
            if (callee_v.tag == NodeTag::Variable) {
                auto callee_name = pool.resolve(callee_v.sym_id);

                // ── (list ...) → nested cons chain ────────────────────
                // Expand (list a b c) to (cons a (cons b (cons c ()))).
                // This avoids evaluator primitive dispatch and works in AOT.
                if (callee_name == "list") {
                    auto result_slot = state.alloc_local();
                    auto arg_count = v.children.size() - 1;
                    if (arg_count == 0) {
                        state.emit(IROpcode::ConstI64, result_slot, 0, 0);
                    } else {
                        // Build from the last arg backwards: cons(cdr, ())
                        auto tail_slot = state.alloc_local();
                        state.emit(IROpcode::ConstI64, tail_slot, 0, 0);
                        for (std::size_t i = arg_count; i >= 1; --i) {
                            auto val =
                                lower_flat_expr(state, flat, pool, v.child(i), cache, cache_hits);
                            auto cell = state.alloc_local();
                            state.emit(IROpcode::MakePair, cell, val, tail_slot);
                            if (i == 1) {
                                state.emit(IROpcode::Local, result_slot, cell);
                            }
                            tail_slot = cell;
                        }
                    }
                    return result_slot;
                }

                // ── (with-arena (size) body ...) ──────────────────────
                if (callee_name == "with-arena") {
                    auto result_slot = state.alloc_local();
                    std::uint32_t arena_size = 65536; // default 64KB
                    std::uint32_t body_start = 1;

                    // Parse optional (size) argument
                    if (v.children.size() >= 2) {
                        auto first_arg = v.child(1);
                        auto first_v = flat.get(first_arg);
                        if (first_v.tag == NodeTag::Call && first_v.children.size() >= 1) {
                            auto size_expr = first_v.child(0);
                            auto sv = flat.get(size_expr);
                            if (sv.tag == NodeTag::LiteralInt && sv.int_value > 0)
                                arena_size = static_cast<std::uint32_t>(sv.int_value);
                            body_start = 2; // (with-arena (N) body...)
                        }
                    }

                    auto saved_slot = state.alloc_local();
                    state.emit(IROpcode::ArenaPush, saved_slot, arena_size, 0, 0);

                    // Evaluate all body expressions, keep last result
                    auto last_slot = result_slot;
                    for (auto ci = body_start; ci < v.children.size(); ++ci) {
                        last_slot =
                            lower_flat_expr(state, flat, pool, v.child(ci), cache, cache_hits);
                    }
                    if (body_start >= v.children.size()) {
                        state.emit(IROpcode::ConstVoid, result_slot);
                    } else {
                        state.emit(IROpcode::Local, result_slot, last_slot);
                    }
                    state.emit(IROpcode::ArenaPop, saved_slot, 0, 0, 0);
                    return result_slot;
                }

                // ── (performance-region body ...) → set region flag ──────
                if (callee_name == "performance-region") {
                    state.region = aura::ir::Region::Performance;
                    auto result_slot = state.alloc_local();
                    if (v.children.size() <= 1) {
                        state.emit(IROpcode::ConstVoid, result_slot);
                    } else {
                        auto last_slot = result_slot;
                        for (std::size_t ci = 1; ci < v.children.size(); ++ci) {
                            last_slot =
                                lower_flat_expr(state, flat, pool, v.child(ci), cache, cache_hits);
                        }
                        state.emit(IROpcode::Local, result_slot, last_slot);
                    }
                    return result_slot;
                }

                // ── (evolution-region body ...) → set region flag ────────
                if (callee_name == "evolution-region") {
                    state.region = aura::ir::Region::Evolution;
                    auto result_slot = state.alloc_local();
                    if (v.children.size() <= 1) {
                        state.emit(IROpcode::ConstVoid, result_slot);
                    } else {
                        auto last_slot = result_slot;
                        for (std::size_t ci = 1; ci < v.children.size(); ++ci) {
                            last_slot =
                                lower_flat_expr(state, flat, pool, v.child(ci), cache, cache_hits);
                        }
                        state.emit(IROpcode::Local, result_slot, last_slot);
                    }
                    return result_slot;
                }

                // Issue #893: fixed table + string_view compare (no std::string temp /
                // unordered_map).
                static constexpr std::pair<std::string_view, IROpcode> kPrimOps[] = {
                    {"+", IROpcode::Add},
                    {"-", IROpcode::Sub},
                    {"*", IROpcode::Mul},
                    {"/", IROpcode::Div},
                    {"=", IROpcode::Eq},
                    {"<", IROpcode::Lt},
                    {">", IROpcode::Gt},
                    {"<=", IROpcode::Le},
                    {">=", IROpcode::Ge},
                    // Issue #1137: do NOT lower equal? to Eq — Eq is fixnum/pointer
                    // equality and historically conflated int 0 with empty-list void.
                    // equal? stays a call to the deep-equality primitive.
                    {"eq?", IROpcode::Eq},
                    {"eqv?", IROpcode::Eq},
                    {"cons", IROpcode::MakePair},
                    {"car", IROpcode::Car},
                    {"cdr", IROpcode::Cdr},
                };
                const IROpcode* found_op = nullptr;
                for (const auto& e : kPrimOps) {
                    if (e.first == callee_name) {
                        found_op = &e.second;
                        break;
                    }
                }
                if (found_op) {
                    auto op = *found_op;
                    auto result_slot = state.alloc_local();
                    auto arg_count = v.children.size() - 1;
                    if (arg_count == 0) {
                        // No args: return 0
                        state.emit(IROpcode::ConstI64, result_slot, 0, 0);
                    } else {
                        // Lower first arg
                        auto prev =
                            lower_flat_expr(state, flat, pool, v.child(1), cache, cache_hits);
                        if (arg_count == 1) {
                            // Unary minus: emit Sub(0, arg)
                            if (callee_name == "-") {
                                auto zero = state.alloc_local();
                                state.emit(IROpcode::ConstI64, zero, 0, 0);
                                state.emit(op, result_slot, zero, prev);
                            } else {
                                state.emit(op, result_slot, prev, prev);
                            }
                        } else if (arg_count == 2) {
                            auto arg1 =
                                lower_flat_expr(state, flat, pool, v.child(2), cache, cache_hits);
                            state.emit(op, result_slot, prev, arg1);
                        } else {
                            // 3+ args: comparison ops need pairwise AND (= a b c → (and (= a b) (=
                            // b c))) arithmetic chains as before: ((a + b) + c)
                            bool is_comp =
                                (op == IROpcode::Eq || op == IROpcode::Lt || op == IROpcode::Gt ||
                                 op == IROpcode::Le || op == IROpcode::Ge);
                            if (is_comp) {
                                // Adjacent pairs ANDed together
                                bool first = true;
                                auto pair_prev = prev;
                                auto and_acc = state.alloc_local();
                                for (std::size_t ai = 2; ai < v.children.size(); ++ai) {
                                    auto pair_next = lower_flat_expr(state, flat, pool, v.child(ai),
                                                                     cache, cache_hits);
                                    auto cmp = state.alloc_local();
                                    state.emit(op, cmp, pair_prev, pair_next);
                                    if (first) {
                                        state.emit(IROpcode::And, and_acc, cmp, cmp);
                                        first = false;
                                    } else {
                                        auto t = state.alloc_local();
                                        state.emit(IROpcode::And, t, and_acc, cmp);
                                        and_acc = t;
                                    }
                                    pair_prev = pair_next;
                                }
                                state.emit(IROpcode::And, result_slot, and_acc, and_acc);
                            } else {
                                // Arithmetic chaining: ((a + b) + c)
                                auto arg1 = lower_flat_expr(state, flat, pool, v.child(2), cache,
                                                            cache_hits);
                                auto acc = state.alloc_local();
                                state.emit(op, acc, prev, arg1);
                                for (std::size_t ai = 3; ai < v.children.size(); ++ai) {
                                    auto next = lower_flat_expr(state, flat, pool, v.child(ai),
                                                                cache, cache_hits);
                                    auto is_last = (ai + 1 == v.children.size());
                                    if (is_last) {
                                        state.emit(op, result_slot, acc, next);
                                    } else {
                                        auto tmp = state.alloc_local();
                                        state.emit(op, tmp, acc, next);
                                        acc = tmp;
                                    }
                                }
                            }
                        }
                    }
                    return result_slot;
                }

                // Handle try/catch special form
                if (callee_name == "try") {
                    // (try body (catch (var) handler))
                    if (v.children.size() < 2) {
                        auto slot = state.alloc_local();
                        state.emit(IROpcode::ConstI64, slot, 0, 0);
                        return slot;
                    }
                    auto body_slot =
                        lower_flat_expr(state, flat, pool, v.child(1), cache, cache_hits);
                    auto is_err_slot = state.alloc_local();
                    state.emit(IROpcode::IsError, is_err_slot, body_slot);

                    auto handler_blk = state.alloc_block();
                    auto continue_blk = state.alloc_block();
                    auto end_blk = state.alloc_block();
                    state.emit(IROpcode::Branch, is_err_slot, handler_blk, continue_blk);

                    // Continue block: body succeeded, pass through
                    state.cur_block = continue_blk;
                    auto phi_slot = state.alloc_local();
                    state.emit(IROpcode::Local, phi_slot, body_slot);
                    state.emit(IROpcode::Jump, end_blk);

                    // Handler block: find catch clause and execute
                    state.cur_block = handler_blk;
                    bool caught = false;
                    for (std::size_t ci = 2; ci < v.children.size(); ++ci) {
                        auto catch_id = v.child(ci);
                        auto catch_v = flat.get(catch_id);
                        if (catch_v.tag == NodeTag::Call) {
                            auto catch_callee = flat.get(catch_v.child(0));
                            if (catch_callee.tag == NodeTag::Variable) {
                                auto catch_name = pool.resolve(catch_callee.sym_id);
                                if (std::string(catch_name) == "catch") {
                                    // (catch (var) handler-body)
                                    if (catch_v.children.size() >= 3) {
                                        auto var_form = flat.get(catch_v.child(1));
                                        std::string var_name;
                                        if (var_form.tag == NodeTag::Call &&
                                            var_form.children.size() >= 1) {
                                            auto var_node = flat.get(var_form.child(0));
                                            if (var_node.tag == NodeTag::Variable)
                                                var_name =
                                                    std::string(pool.resolve(var_node.sym_id));
                                        }
                                        // Allocate slot and bind error value
                                        auto err_slot = state.alloc_local();
                                        state.emit(IROpcode::Local, err_slot, body_slot);
                                        // Ensure a scope exists for the error variable binding
                                        if (state.scopes.empty()) {
                                            state.scopes.push_back({});
                                        }
                                        if (!var_name.empty()) {
                                            state.scopes.back()[var_name] =
                                                Binding{BindingKind::Local, err_slot};
                                        }
                                        auto handler_slot = lower_flat_expr(
                                            state, flat, pool, catch_v.child(2), cache, cache_hits);
                                        state.emit(IROpcode::Local, phi_slot, handler_slot);
                                        caught = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (!caught) {
                        // No catch clause: re-raise by passing error through
                        state.emit(IROpcode::Local, phi_slot, body_slot);
                    }
                    state.emit(IROpcode::Jump, end_blk);

                    state.cur_block = end_blk;
                    return phi_slot;
                }

                // Pre-executed require/import/use: skip (already handled by pre_exec_requires)
                if (callee_name == "require" || callee_name == "import") {
                    // import/require are handled by pre_exec_requires.
                    // The import call in the AST is still present; skip
                    // it here to avoid going through PrimCall dispatch.
                    auto slot = state.alloc_local();
                    state.emit(IROpcode::ConstVoid, slot);
                    return slot;
                }

                // Check if callee is a known non-arithmetic primitive (string ops, etc.)
                static const std::unordered_map<std::string, PrimId> prim_call_map = {
                    {"hash", PrimId::Hash},
                    {"hash-length", PrimId::HashLength},
                    {"hash-has-key?", PrimId::HashHasKey},
                    {"hash-keys", PrimId::HashKeys},
                    {"hash-values", PrimId::HashValues},
                    {"string-append", PrimId::StringAppend},
                    {"string-length", PrimId::StringLength},
                    {"string-ref", PrimId::StringRef},
                    {"substring", PrimId::Substring},
                    {"string=?", PrimId::StringEq},
                    {"string<?", PrimId::StringLt},
                    {"number->string", PrimId::NumberToString},
                    {"string->number", PrimId::StringToNumber},
                    {"display", PrimId::Display},
                    {"write", PrimId::Write},
                    {"newline", PrimId::Newline},
                    {"error", PrimId::Error},
                    {"assert", PrimId::Assert},
                    {"raise", PrimId::Raise},
                    {"error?", PrimId::ErrorP},
                    {"read", PrimId::Read},
                    {"read-file", PrimId::ReadFile},
                    {"write-file", PrimId::WriteFile},
                    {"file-exists?", PrimId::FileExists},
                    {"gensym", PrimId::Gensym},
                    {"apply", PrimId::Apply},
                    {"vector", PrimId::Vector},
                    {"vector-ref", PrimId::VectorRef},
                    {"vector-set!", PrimId::VectorSet},
                    {"vector-length", PrimId::VectorLength},
                    {"vector?", PrimId::VectorP},
                    {"make-vector", PrimId::MakeVector},
                    {"import", PrimId::Import},
                    {"char=?", PrimId::CharEq},
                    {"char<?", PrimId::CharLt},
                    {"char->integer", PrimId::CharToInteger},
                    {"integer->char", PrimId::IntegerToChar},
                    {"quotient", PrimId::Quotient},
                    {"remainder", PrimId::Remainder},
                    {"length", PrimId::ListLength},
                    {"list-ref", PrimId::ListRef},
                    {"reverse", PrimId::ListReverse},
                    {"pair?", PrimId::PairP},
                    {"null?", PrimId::NullP},
                };
                // Hash operations: emit inline opcodes (avoid PrimCall dispatch)
                static const std::unordered_map<std::string, IROpcode> hash_op_map = {
                    {"hash-ref", IROpcode::HashRef},
                    {"hash-set!", IROpcode::HashSet},
                    {"hash-remove!", IROpcode::HashRemove},
                };
                auto hop = hash_op_map.find(std::string(callee_name));
                if (hop != hash_op_map.end()) {
                    auto result_slot = state.alloc_local();
                    auto hash_slot =
                        lower_flat_expr(state, flat, pool, v.child(1), cache, cache_hits);
                    auto key_slot =
                        lower_flat_expr(state, flat, pool, v.child(2), cache, cache_hits);
                    if (v.children.size() >= 4) {
                        auto val_slot =
                            lower_flat_expr(state, flat, pool, v.child(3), cache, cache_hits);
                        auto pair_slot = state.alloc_local();
                        state.emit(IROpcode::MakePair, pair_slot, key_slot, val_slot);
                        state.emit(hop->second, result_slot, hash_slot, pair_slot);
                    } else {
                        state.emit(hop->second, result_slot, hash_slot, key_slot);
                    }
                    return result_slot;
                }


                auto pcit = prim_call_map.find(std::string(callee_name));
                if (pcit != prim_call_map.end()) {
                    auto prim_id = static_cast<std::uint32_t>(pcit->second);
                    auto arg_count = static_cast<std::uint32_t>(v.children.size() - 1);
                    auto arg_base = state.local_count;
                    for (std::size_t i = 1; i < v.children.size(); ++i) {
                        auto val_slot =
                            lower_flat_expr(state, flat, pool, v.child(i), cache, cache_hits);
                        state.emit(IROpcode::Local, arg_base + static_cast<std::uint32_t>(i - 1),
                                   val_slot);
                        state.alloc_local();
                    }
                    auto result_slot = state.alloc_local();
                    state.emit(IROpcode::PrimCall, prim_id, arg_base, arg_count, result_slot);
                    return result_slot;
                }

                // ── Expand cond to nested if/Branch IR ─────────────────
                // (cond (p1 b1) (p2 b2) ... (else bn))
                // → if p1: b1; elif p2: b2; ... else: bn
                if (callee_name == "cond" && v.children.size() >= 2) {
                    auto result_slot = state.alloc_local();
                    auto done_blk = decltype(state.alloc_block()){};
                    auto num_clauses = v.children.size() - 1;

                    for (std::size_t ci = 1; ci <= num_clauses; ++ci) {
                        auto clause = v.child(ci);
                        if (clause >= flat.size())
                            continue;
                        auto clause_node = flat.get(clause);
                        if (clause_node.children.empty())
                            continue;

                        // Clause: child[0] = predicate, child[1] = body (single expression)
                        auto pred_node = clause_node.child(0);
                        auto body_node = (clause_node.children.size() > 1) ? clause_node.child(1)
                                                                           : clause_node.child(0);

                        bool is_last = (ci == num_clauses);

                        // Lower predicate (or check if it's else/#t)
                        // For the last clause with #t predicate (else), skip the branch
                        auto pred_slot =
                            lower_flat_expr(state, flat, pool, pred_node, cache, cache_hits);

                        if (!is_last) {
                            // Branch on predicate: if true → body, else → next clause
                            auto body_blk = state.alloc_block();
                            auto next_blk = state.alloc_block();
                            state.emit(IROpcode::Branch, pred_slot, body_blk, next_blk);

                            // Body block
                            state.cur_block = body_blk;
                            auto body_val =
                                lower_flat_expr(state, flat, pool, body_node, cache, cache_hits);
                            state.emit(IROpcode::Local, result_slot, body_val);
                            if (!done_blk)
                                done_blk = state.alloc_block();
                            state.emit(IROpcode::Jump, done_blk);

                            // Next clause
                            state.cur_block = next_blk;
                        } else {
                            // Last clause: fallthrough body
                            auto body_val =
                                lower_flat_expr(state, flat, pool, body_node, cache, cache_hits);
                            state.emit(IROpcode::Local, result_slot, body_val);
                            if (!done_blk)
                                done_blk = state.alloc_block();
                            state.emit(IROpcode::Jump, done_blk);
                        }
                    }

                    state.cur_block = done_blk;
                    return result_slot;
                }

                // ── Expand and/or/not to IR opcodes ───────────────────────
                // These must NOT go through OpPrimitive + OpCall (which requires
                // the evaluator runtime for primitive dispatch). AOT binaries
                // can only handle direct IR instructions.
                //
                // not: IROpcode::Not (inlined in LLVM builder)
                // and: short-circuit via Branch + ConstVoid + Jump pattern
                // or:  short-circuit via Branch + Local + Jump pattern
                //
                // not: inlined as IROpcode::Not (expanded to LLVM ICmp + Xor)
                if (callee_name == "not" && v.children.size() == 2) {
                    auto arg_slot =
                        lower_flat_expr(state, flat, pool, v.child(1), cache, cache_hits);
                    auto result_slot = state.alloc_local();
                    state.emit(IROpcode::Not, result_slot, arg_slot);
                    return result_slot;
                }

                if (callee_name == "and" && v.children.size() >= 2) {
                    // Expand (and e1 e2 ... en) to:
                    //   block0:
                    //     val = lower(e1)
                    //     branch val → block_ok1, block_nope1
                    //   block_nope1: result = 0; jump block_done
                    //   block_ok1:
                    //     val = lower(e2)
                    //     branch val → block_ok2, block_nope2
                    //   ...
                    //   block_okN: (last expr)
                    //     result = lower(en)
                    //     jump block_done
                    //   block_done: return result
                    auto arg_count = static_cast<std::uint32_t>(v.children.size() - 1);
                    auto result_slot = state.alloc_local();
                    auto done_blk = decltype(state.alloc_block()){};

                    for (std::size_t i = 1; i < arg_count; ++i) {
                        auto val =
                            lower_flat_expr(state, flat, pool, v.child(i), cache, cache_hits);
                        auto nope_blk = state.alloc_block();
                        auto ok_blk = state.alloc_block();
                        state.emit(IROpcode::Branch, val, ok_blk, nope_blk);

                        // nope block: result = 0, jump done
                        state.cur_block = nope_blk;
                        state.emit(IROpcode::ConstVoid, result_slot);
                        if (!done_blk)
                            done_blk = state.alloc_block();
                        state.emit(IROpcode::Jump, done_blk);

                        // ok block: continue to next expression
                        state.cur_block = ok_blk;
                    }

                    // Last expression: result = expression value, jump done
                    {
                        auto last_slot = lower_flat_expr(state, flat, pool, v.child(arg_count),
                                                         cache, cache_hits);
                        state.emit(IROpcode::Local, result_slot, last_slot);
                        if (!done_blk)
                            done_blk = state.alloc_block();
                        state.emit(IROpcode::Jump, done_blk);
                    }

                    // Done block
                    state.cur_block = done_blk;
                    return result_slot;
                }

                if (callee_name == "or" && v.children.size() >= 2) {
                    // Expand (or e1 e2 ... en) to:
                    //   block0:
                    //     val = lower(e1)
                    //     branch val → block_done_with_val, block_next
                    //   block_next:
                    //     val = lower(e2)
                    //     branch val → block_done_with_val, block_next2
                    //   ...
                    //   block_last:
                    //     result = lower(en)
                    //     jump block_done
                    //   block_done_with_val_i:
                    //     result = val_i
                    //     jump block_done
                    //   block_done: return result
                    auto arg_count = static_cast<std::uint32_t>(v.children.size() - 1);
                    auto result_slot = state.alloc_local();
                    auto done_blk = decltype(state.alloc_block()){};

                    for (std::size_t i = 1; i < arg_count; ++i) {
                        auto val =
                            lower_flat_expr(state, flat, pool, v.child(i), cache, cache_hits);
                        auto done_val_blk = state.alloc_block();
                        auto next_blk = state.alloc_block();
                        state.emit(IROpcode::Branch, val, done_val_blk, next_blk);

                        // done_val block: result = val, jump done
                        state.cur_block = done_val_blk;
                        state.emit(IROpcode::Local, result_slot, val);
                        if (!done_blk)
                            done_blk = state.alloc_block();
                        state.emit(IROpcode::Jump, done_blk);

                        // next block: evaluate next expression
                        state.cur_block = next_blk;
                    }

                    // Last expression: result = expression value, jump done
                    {
                        auto last_slot = lower_flat_expr(state, flat, pool, v.child(arg_count),
                                                         cache, cache_hits);
                        state.emit(IROpcode::Local, result_slot, last_slot);
                        if (!done_blk)
                            done_blk = state.alloc_block();
                        state.emit(IROpcode::Jump, done_blk);
                    }

                    // Done block
                    state.cur_block = done_blk;
                    return result_slot;
                }

                // Check if callee is a cached define function (now handled by Variable handler)
                // Fall through to general function call path
            }

            // General function call: lower callee and args, then emit Call
            auto callee_slot = lower_flat_expr(state, flat, pool, v.child(0), cache, cache_hits);
            auto arg_count = static_cast<std::uint32_t>(v.children.size() - 1);

            // Try to get callee's expected parameter types for coercion (P0)
            aura::core::FuncType callee_func_type;
            bool have_callee_type = false;
            if (state.type_reg) {
                auto callee_type_id = flat.type_id(v.child(0));
                if (callee_type_id != 0) {
                    auto ctid = aura::core::TypeId{callee_type_id, 1};
                    if (auto* ft = state.type_reg->func_of(ctid)) {
                        callee_func_type = *ft;
                        have_callee_type = true;
                    }
                }
            }

            // Reserve contiguous argument block
            auto arg_base = state.local_count;
            for (std::size_t i = 1; i < v.children.size(); ++i) {
                auto arg_node = v.child(i);
                auto val_slot = lower_flat_expr(state, flat, pool, arg_node, cache, cache_hits);
                // Insert CastOp if arg type differs from expected param type
                if (have_callee_type && (i - 1) < callee_func_type.args.size()) {
                    auto arg_type = flat.type_id(arg_node);
                    if (arg_type != 0) {
                        auto expected_type = callee_func_type.args[i - 1];
                        if (arg_type != expected_type.index) {
                            auto cast_slot = state.alloc_local();
                            auto cast_tag = type_tag_for_coercion(expected_type, state.type_reg);
                            state.emit_with_type(IROpcode::CastOp, expected_type.index, cast_slot,
                                                 val_slot, cast_tag, 0);
                            state.emit(IROpcode::Local,
                                       arg_base + static_cast<std::uint32_t>(i - 1), cast_slot);
                            state.alloc_local();
                            continue; // skip the normal Local emit below
                        }
                    }
                }
                state.emit(IROpcode::Local, arg_base + static_cast<std::uint32_t>(i - 1), val_slot);
                state.alloc_local();
            }
            auto result_slot = state.alloc_local();
            // Call(callee_slot, arg_base, arg_count, result_slot)
            state.emit(IROpcode::Call, callee_slot, arg_base, arg_count, result_slot);
            return result_slot;
        }
        case NodeTag::IfExpr: {
            auto cond_slot = lower_flat_expr(state, flat, pool, v.child(0), cache, cache_hits);
            auto then_blk = state.alloc_block();
            auto else_blk = state.alloc_block();
            auto merge_blk = state.alloc_block();

            // Issue #280: if the IfExpr's narrowing evidence is
            // set on the lowering state (from TypeCheckWrap's last
            // check_before_lowering), attach it to the Branch via
            // emit_with_metadata. The narrowing bitmask tells
            // DeadCoercionEliminationPass / JIT which predicate(s)
            // statically guarantee the then-branch's refinement.
            // Default 0 = no hint; emit() is used instead of
            // emit_with_metadata to avoid touching the
            // narrow_evidence field when there's no narrowing.
            if (state.current_narrowing_evidence != 0) {
                state.emit_with_metadata(IROpcode::Branch, 0, 0, 0,
                                         state.current_narrowing_evidence, cond_slot, then_blk,
                                         else_blk);
                // Reset so nested IfExprs don't inherit the
                // narrowing hint from their enclosing one.
                state.current_narrowing_evidence = 0;
            } else {
                state.emit(IROpcode::Branch, cond_slot, then_blk, else_blk);
            }

            auto phi_slot = state.alloc_local();

            state.cur_block = then_blk;
            auto then_val = lower_flat_expr(state, flat, pool, v.child(1), cache, cache_hits);
            state.emit(IROpcode::Local, phi_slot, then_val);
            state.emit(IROpcode::Jump, merge_blk);

            state.cur_block = else_blk;
            auto else_val = lower_flat_expr(state, flat, pool, v.child(2), cache, cache_hits);
            state.emit(IROpcode::Local, phi_slot, else_val);
            state.emit(IROpcode::Jump, merge_blk);

            state.cur_block = merge_blk;
            return phi_slot;
        }
        case NodeTag::Lambda: {
            // Native lambda lowering: create a new IRFunction, lower body
            // via lower_flat_expr, emit MakeClosure.
            auto param_span = v.params;
            std::vector<std::string> param_names;
            for (auto pid : param_span)
                param_names.push_back(std::string(pool.resolve(pid)));

            // Collect free variables: vars in body not in param list
            std::unordered_set<std::string> free_set;
            std::unordered_set<std::string> bound(param_names.begin(), param_names.end());
            // Walk body to find free vars
            {
                struct FreeVarWalker {
                    const FlatAST& flat;
                    StringPool& pool;
                    std::unordered_set<std::string>& free;
                    std::unordered_set<std::string>& bound;
                    void walk(NodeId nid) {
                        if (nid == NULL_NODE || nid >= flat.size())
                            return;
                        auto nv = flat.get(nid);
                        switch (nv.tag) {
                            case NodeTag::Variable: {
                                auto name = pool.resolve(nv.sym_id);
                                if (bound.find(std::string(name)) == bound.end())
                                    free.insert(std::string(name));
                                break;
                            }
                            case NodeTag::LiteralInt:
                            case NodeTag::LiteralString:
                                break;
                            default:
                                for (auto c : nv.children)
                                    walk(c);
                                break;
                        }
                    }
                };
                FreeVarWalker{flat, pool, free_set, bound}.walk(v.child(0));
            }

            // Filter to only vars actually in scope
            std::vector<std::string> free_vars;
            for (auto& fv : free_set) {
                bool in_scope = false;
                for (auto it = state.scopes.rbegin(); it != state.scopes.rend(); ++it) {
                    if (it->find(fv) != it->end()) {
                        in_scope = true;
                        break;
                    }
                }
                if (in_scope)
                    free_vars.push_back(fv);
            }

            // Create new IR function
            IRFunction func;
            func.name = "__lambda__";
            func.entry_block = 0;
            func.blocks.push_back({0, {}, {}});
            func.params = param_names;
            func.arg_count = static_cast<std::uint32_t>(param_names.size());
            func.variadic = (v.int_value != 0);
            func.region = state.region;
            // Issue #150 Phase 1b: if the user wrapped this
            // lambda in a (performance-region ...) or
            // (evolution-region ...) Aura form, the parser
            // recorded the region in the FlatAST's
            // region_by_lambda_id_ side-table. Look it up and
            // override the IRFunction's region. This is the
            // write-side that makes the annotation take effect:
            // without this lookup, the parser-side hint
            // (Phase 1) is just data in a side-table that
            // nothing consumes.
            if (auto opt_r = flat.get_function_region_for_lambda(id)) {
                func.region = static_cast<aura::ir::Region>(*opt_r);
            }

            // Save parent state
            auto* saved_func = state.cur_func;
            auto saved_block = state.cur_block;
            auto saved_locals = state.local_count;
            auto saved_scopes = std::move(state.scopes);
            auto saved_env_slot = state.env_slot;
            auto saved_fv_map = std::move(state.free_var_map);
            auto saved_self_func_id = state.self_func_id;

            state.cur_func = &func;
            state.cur_block = 0;
            state.local_count = 0;
            state.free_var_map.clear();

            state.env_slot = state.alloc_local(); // placeholder
            state.scopes.push_back({});

            // Load captured free vars from env prefix
            for (std::size_t i = 0; i < free_vars.size(); ++i) {
                auto& fv = free_vars[i];
                auto slot = state.alloc_local();
                state.emit(IROpcode::Arg, slot, static_cast<std::uint32_t>(i));
                // Check if this free var is a letrec cell in the outer scope
                bool is_cell = false;
                for (auto it = saved_scopes.rbegin(); it != saved_scopes.rend(); ++it) {
                    auto found = it->find(fv);
                    if (found != it->end() && found->second.kind == BindingKind::Cell) {
                        is_cell = true;
                        break;
                    }
                }
                if (is_cell) {
                    // Cell capture: keep the binding as Cell so that:
                    // - read access uses CellGet (dereferences via cell_heap_)
                    // - write access uses CellSet (updates shared cell)
                    // CaptureRef stores the cell_index in closure env; Arg
                    // decodes it back to cell_index; CellGet/CellSet use it.
                    state.scopes.back()[fv] = Binding{BindingKind::Cell, slot};
                    state.free_var_map[fv] = static_cast<std::uint32_t>(i);
                    state.cell_free_vars.insert(fv);
                } else {
                    state.scopes.back()[fv] = Binding{BindingKind::Captured, slot};
                    state.free_var_map[fv] = static_cast<std::uint32_t>(i);
                }
            }

            // Bind parameters
            for (std::size_t i = 0; i < param_names.size(); ++i) {
                auto slot = state.alloc_local();
                state.emit(IROpcode::Arg, slot, static_cast<std::uint32_t>(free_vars.size() + i));
                state.scopes.back()[param_names[i]] = Binding{BindingKind::Local, slot};
            }

            // Lower body via native FlatAST path
            auto body_slot = lower_flat_expr(state, flat, pool, v.child(0), cache, cache_hits);
            state.emit(IROpcode::Return, body_slot);

            func.local_count = state.local_count;

            // Restore parent state
            state.cur_func = saved_func;
            state.cur_block = saved_block;
            state.local_count = saved_locals;
            state.scopes = std::move(saved_scopes);
            state.env_slot = saved_env_slot;
            state.free_var_map = std::move(saved_fv_map);
            state.self_func_id = saved_self_func_id;

            func.free_vars = free_vars;
            if (!state.current_flat)
                std::println("DEBUG: current_flat NULL at lambda bridge data");
            if (!state.current_pool)
                std::println("DEBUG: current_pool NULL at lambda bridge data");
            auto fid = state.module.add_function(std::move(func));
            // Issue #246: propagate the SyntaxMarker from the
            // source Lambda node into the IRFunction. The
            // inliner consults this to apply macro-hygiene
            // policy (skip inlining into/from macro-introduced
            // code by default).
            if (state.current_flat) {
                state.module.functions[fid].marker =
                    static_cast<std::uint8_t>(state.current_flat->marker(v.id));
            }
            // Store bridge data for tree-walker compatibility
            if (state.current_flat && state.current_pool) {
                // Issue #224 Cycle 2: shared_ptr-based bridge. The
                // shared_ptr is a non-owning view (no-op deleter) that
                // keeps the FlatAST/StringPool alive as long as this
                // bridge exists. The arena remains the actual owner.
                auto flat_sp = std::shared_ptr<const aura::ast::FlatAST>(
                    state.current_flat, [](const aura::ast::FlatAST*) {});
                auto pool_sp = std::shared_ptr<const aura::ast::StringPool>(
                    state.current_pool, [](const aura::ast::StringPool*) {});
                state.module.set_closure_bridge(fid, std::move(flat_sp), std::move(pool_sp),
                                                v.child(0));
                if (g_lowering_hooks.on_quote_lambda_bridge_copy)
                    g_lowering_hooks.on_quote_lambda_bridge_copy();
                if (g_lowering_hooks.on_env_version_resync)
                    g_lowering_hooks.on_env_version_resync();
                // Save lambda body source for bridge fallback re-parse
                auto body_src = unparse_node(*state.current_flat, *state.current_pool, v.child(0));
                state.module.set_closure_body_source(fid, body_src);
            }
            auto slot = state.alloc_local();
            state.emit(IROpcode::MakeClosure, slot, fid,
                       static_cast<std::uint32_t>(free_vars.size()));

            // Capture each free variable into the closure
            // Cell values (CellRef) are captured directly — CellGet/CellSet
            // in the lambda body dereference via cell_heap_ for read/write.
            for (std::size_t i = 0; i < free_vars.size(); ++i) {
                auto& fv = free_vars[i];
                for (auto it = state.scopes.rbegin(); it != state.scopes.rend(); ++it) {
                    auto found = it->find(fv);
                    if (found != it->end()) {
                        auto& binding = found->second;
                        auto cslot = state.alloc_local();
                        state.emit(IROpcode::Local, cslot, binding.slot);
                        state.emit(IROpcode::Capture, slot, static_cast<std::uint32_t>(i), cslot);
                        break;
                    }
                }
            }

            return slot;
        }
        case NodeTag::Let:
        case NodeTag::LetRec: {
            bool rec = (v.tag == NodeTag::LetRec);
            auto name = pool.resolve(v.sym_id);
            auto val_id = v.children.empty() ? NULL_NODE : v.child(0);
            auto body_id = v.children.size() < 2 ? NULL_NODE : v.child(1);

            if (rec) {
                // letrec: create cell, bind, init
                auto ci = state.alloc_local();
                state.emit(IROpcode::NewCell, ci);
                state.scopes.push_back({});
                auto& scope = state.scopes.back();
                scope[std::string(name)] = Binding{BindingKind::Cell, ci};
                auto val_slot = lower_flat_expr(state, flat, pool, val_id, cache, cache_hits);
                state.emit(IROpcode::CellSet, ci, val_slot);
                auto body_slot = lower_flat_expr(state, flat, pool, body_id, cache, cache_hits);
                state.scopes.pop_back();
                return body_slot;
            } else {
                // let: use Cell binding (like evaluator fix 3392d77)
                // so that set! can find the mutable Cell inside closures
                auto ci = state.alloc_local();
                state.emit(IROpcode::NewCell, ci);
                auto val_slot = lower_flat_expr(state, flat, pool, val_id, cache, cache_hits);
                state.emit(IROpcode::CellSet, ci, val_slot);
                state.scopes.push_back({});
                auto& scope = state.scopes.back();
                scope[std::string(name)] = Binding{BindingKind::Cell, ci};
                auto body_slot = lower_flat_expr(state, flat, pool, body_id, cache, cache_hits);
                state.scopes.pop_back();
                return body_slot;
            }
        }
        case NodeTag::Define: {
            auto name = pool.resolve(v.sym_id);
            auto val_id = v.children.empty() ? NULL_NODE : v.child(0);
            auto& scope = state.scopes.empty() ? (state.scopes.push_back({}), state.scopes.back())
                                               : state.scopes.back();
            // Check if already pre-bound by Begin handler (for mutual recursion).
            // If a Cell binding exists, reuse it (no NewCell needed).
            auto existing = scope.find(std::string(name));
            bool has_cell = (existing != scope.end() && existing->second.kind == BindingKind::Cell);
            std::uint32_t ci;
            if (has_cell) {
                ci = existing->second.slot;
            } else {
                // Pre-bind the name as a Cell BEFORE lowering the value so that
                // self-references in the lambda body find the cell (like letrec).
                ci = state.alloc_local();
                state.emit(IROpcode::NewCell, ci);
                scope[std::string(name)] = Binding{BindingKind::Cell, ci};
            }
            // Now lower the value — self-references will CellGet the pre-bound cell
            auto val_slot = lower_flat_expr(state, flat, pool, val_id, cache, cache_hits);
            state.emit(IROpcode::CellSet, ci, val_slot);
            return val_slot;
        }
        case NodeTag::Begin: {
            // First pass: pre-bind all define names for mutual recursion support.
            // Each define gets a Cell that all sibling defines can reference.
            if (!state.scopes.empty()) {
                auto& scope = state.scopes.back();
                for (auto c : v.children) {
                    if (c < flat.size() && flat.get(c).tag == NodeTag::Define) {
                        auto name = pool.resolve(flat.get(c).sym_id);
                        if (scope.find(std::string(name)) == scope.end()) {
                            auto ci = state.alloc_local();
                            state.emit(IROpcode::NewCell, ci);
                            scope[std::string(name)] = Binding{BindingKind::Cell, ci};
                        }
                    }
                }
            } else {
                state.scopes.push_back({});
                auto& scope = state.scopes.back();
                for (auto c : v.children) {
                    if (c < flat.size() && flat.get(c).tag == NodeTag::Define) {
                        auto name = pool.resolve(flat.get(c).sym_id);
                        auto ci = state.alloc_local();
                        state.emit(IROpcode::NewCell, ci);
                        scope[std::string(name)] = Binding{BindingKind::Cell, ci};
                    }
                }
            }
            // Second pass: lower all children — defines find their pre-bound cells
            std::uint32_t last_slot = 0;
            for (auto c : v.children)
                last_slot = lower_flat_expr(state, flat, pool, c, cache, cache_hits);
            if (!v.children.empty())
                return last_slot;
            auto slot = state.alloc_local();
            state.emit(IROpcode::ConstI64, slot, 0, 0);
            return slot;
        }
        case NodeTag::Set: {
            auto name = pool.resolve(v.sym_id);
            auto val_id = v.children.empty() ? NULL_NODE : v.child(0);
            auto val_slot = lower_flat_expr(state, flat, pool, val_id, cache, cache_hits);

            for (auto it = state.scopes.rbegin(); it != state.scopes.rend(); ++it) {
                auto found = it->find(std::string(name));
                if (found != it->end()) {
                    auto& binding = found->second;
                    if (binding.kind == BindingKind::Cell) {
                        state.emit(IROpcode::CellSet, binding.slot, val_slot);
                        return val_slot;
                    }
                    // For local vars, just update the slot value
                    state.emit(IROpcode::Local, binding.slot, val_slot);
                    return val_slot;
                }
            }
            return val_slot;
        }
        case NodeTag::Quote: {
            // Inline simple literals
            if (!v.children.empty()) {
                auto cv = flat.get(v.child(0));
                if (cv.tag == NodeTag::LiteralInt) {
                    if (cv.int_value == 0) {
                        // () empty list → make_void
                        auto slot = state.alloc_local();
                        state.emit(IROpcode::ConstVoid, slot);
                        return slot;
                    }
                    auto slot = state.alloc_local();
                    state.emit(IROpcode::ConstI64, slot, cv.int_value, 0);
                    return slot;
                }
                if (cv.tag == NodeTag::LiteralFloat || cv.tag == NodeTag::LiteralString) {
                    return lower_flat_expr(state, flat, pool, v.child(0), cache, cache_hits);
                }
            }
            // Non-trivial quoted data: lower as (cons car cdr) chain
            if (v.children.empty() || !state.primitives) {
                auto slot = state.alloc_local();
                state.emit(IROpcode::ConstI64, slot, 0, 0);
                return slot;
            }

            auto cons_slot = state.primitives->slot_for_name("cons");
            if (cons_slot >= state.primitives->slot_count()) {
                auto slot = state.alloc_local();
                state.emit(IROpcode::ConstI64, slot, 0, 0);
                return slot;
            }
            auto cons_fn = state.alloc_local();
            state.emit(IROpcode::Primitive, cons_fn, static_cast<std::uint32_t>(cons_slot));

            // Recursive helper: lower a quoted value as data
            // For lists/pairs, lower each car/cdr with Quote semantics
            // i.e. (1 2) as data is cons(1, cons(2, 0))
            std::function<std::uint32_t(NodeId)> lower_q;
            lower_q = [&](NodeId nid) -> std::uint32_t {
                if (nid == ast::NULL_NODE || nid >= flat.size()) {
                    auto s = state.alloc_local();
                    state.emit(IROpcode::ConstI64, s, 0, 0);
                    return s;
                }
                auto nv = flat.get(nid);
                // Simple literals
                if (nv.tag == NodeTag::LiteralInt) {
                    auto s = state.alloc_local();
                    state.emit(IROpcode::ConstI64, s, nv.int_value, 0);
                    return s;
                }
                if (nv.tag == NodeTag::LiteralFloat || nv.tag == NodeTag::LiteralString) {
                    return lower_flat_expr(state, flat, pool, nid, cache, cache_hits);
                }
                // Lists: Pair or Call as data
                if (nv.tag == NodeTag::Pair) {
                    // Dotted pair: (car . cdr) → cons(lower_q(car), lower_q(cdr))
                    auto left = nv.children.empty() ? ast::NULL_NODE : nv.child(0);
                    auto right = nv.children.size() > 1 ? nv.child(1) : ast::NULL_NODE;
                    auto left_slot = lower_q(left);
                    auto right_slot = lower_q(right);
                    // Call cons(left, right)
                    auto ab = state.alloc_local();
                    state.emit(IROpcode::Local, ab, left_slot);
                    state.alloc_local();
                    auto cd = state.local_count - 1;
                    state.emit(IROpcode::Local, static_cast<std::uint32_t>(cd), right_slot);
                    auto rs = state.alloc_local();
                    state.emit(IROpcode::Call, cons_fn, ab, 2, rs);
                    return rs;
                }
                if (nv.tag == NodeTag::Call) {
                    // Proper list: (a b c) → cons(a, cons(b, cons(c, 0)))
                    auto tail_s = state.alloc_local();
                    state.emit(IROpcode::ConstVoid, tail_s);
                    for (int ci = static_cast<int>(nv.children.size()) - 1; ci >= 0; --ci) {
                        auto elem_s = lower_q(nv.child(ci));
                        auto ab = state.alloc_local();
                        state.emit(IROpcode::Local, ab, elem_s);
                        state.alloc_local();
                        auto cd = state.local_count - 1;
                        state.emit(IROpcode::Local, static_cast<std::uint32_t>(cd), tail_s);
                        auto rs = state.alloc_local();
                        state.emit(IROpcode::Call, cons_fn, ab, 2, rs);
                        tail_s = rs;
                    }
                    return tail_s;
                }
                // Unknown node type
                return lower_flat_expr(state, flat, pool, nid, cache, cache_hits);
            };

            return lower_q(v.child(0));
        }
        case NodeTag::TypeAnnotation: {
            // Lower the inner expression first
            auto inner_slot = lower_flat_expr(state, flat, pool, v.child(0), cache, cache_hits);
            // Check if coercion is needed: compare annotation type_id with inner type_id
            auto ann_type_id = flat.type_id(id);
            auto inner_type_id = flat.type_id(v.child(0));
            // Emit CastOp for type boundary when:
            // 1. Both types are known and differ (static → static coercion)
            // 2. Annotation is static but inner is dynamic (runtime type check)
            if ((ann_type_id != 0 && inner_type_id != 0 && ann_type_id != inner_type_id) ||
                (ann_type_id != 0 && inner_type_id == 0)) {
                // Emit CastOp for the type boundary
                auto slot = state.alloc_local();
                auto ann_tid = aura::core::TypeId{ann_type_id, 1};
                std::uint32_t type_tag = type_tag_for_coercion(ann_tid, state.type_reg);
                std::uint32_t blame_loc = 0;
                state.emit_with_type(IROpcode::CastOp, ann_type_id, slot, inner_slot, type_tag,
                                     blame_loc);
                return slot;
            }
            return inner_slot;
        }
        case NodeTag::Coercion: {
            auto inner = lower_flat_expr(state, flat, pool, v.child(0), cache, cache_hits);
            std::uint32_t type_tag = static_cast<std::uint32_t>(v.int_value);
            auto target_type_id = flat.type_id(v.id);
            const auto inner_type_id = flat.type_id(v.child(0));
            const std::uint32_t entry_narrow_ev =
                v.float_value != 0.0 ? static_cast<std::uint32_t>(v.float_value) : 0u;
            const std::uint32_t narrow_ev =
                entry_narrow_ev != 0 ? entry_narrow_ev : state.current_narrowing_evidence;
            // Issue #691: post-narrow cast elision when evidence + concrete types match.
            if (narrow_ev != 0 && target_type_id != 0 && inner_type_id != 0 &&
                target_type_id == inner_type_id) {
                return inner;
            }
            auto slot = state.alloc_local();
            std::uint32_t blame_loc = (static_cast<std::uint32_t>(v.line) << 16) |
                                      (static_cast<std::uint32_t>(v.col) & 0xFFFFu);
            // CastOp type_id = coercion target type (stored on the Coercion node)
            // blame info is carried via blame_loc operand and source_id, not type_id
            // Issue #629 / #691: attach narrow_evidence to coercion CastOps
            // from entry provenance or enclosing if-branch context.
            if (narrow_ev != 0) {
                state.emit_with_metadata(IROpcode::CastOp, target_type_id, 0, 0, narrow_ev, slot,
                                         inner, type_tag, blame_loc);
            } else {
                state.emit_with_type(IROpcode::CastOp, target_type_id, slot, inner, type_tag,
                                     blame_loc);
            }
            return slot;
        }
        case NodeTag::Linear:
        case NodeTag::Move:
        case NodeTag::Borrow:
        case NodeTag::MutBorrow:
        case NodeTag::Drop: {
            // Issue #133: linear type lowering extracted to
            // try_lower_linear_type. The callback
            // recurses into the inner expression via the
            // main lower_flat_expr.
            if (auto slot = try_lower_linear_type(state, flat, pool, v, [&](aura::ast::NodeId id) {
                    return lower_flat_expr(state, flat, pool, id, cache, cache_hits);
                })) {
                return *slot;
            }
            // Shouldn't happen for these tags, but fall
            // through to the default case just in case.
            auto slot = state.alloc_local();
            state.emit(IROpcode::ConstI64, slot, 0, 0);
            return slot;
        }
        case NodeTag::MacroDef:
        default: {
            // Macros handled by evaluator fallback; emit 0
            auto slot = state.alloc_local();
            state.emit(IROpcode::ConstI64, slot, 0, 0);
            return slot;
        }
    }
}

// ── Issue #150 Phase 2: automatic region inference helpers ─
// Walk the FlatAST collecting all node ids that are calls
// to mutation primitives (mutate:*, ast:*, current-eval).
// The set is then used by the inference loop in
// lower_to_ir_impl to flag Define / Lambda bodies that
// contain these nodes as Evolution.
//
// Issue #896 Phase 2: generation-stamped thread_local cache so
// repeated lowerings of the same FlatAST skip the O(N) rescan.
static void collect_mutation_calls(const FlatAST& flat,
                                   std::unordered_set<aura::ast::NodeId>& out) {
    for (aura::ast::NodeId i = 0; i < flat.size(); ++i) {
        if (i >= flat.size())
            break;
        auto v = flat.get(i);
        if (v.tag != aura::ast::NodeTag::Call)
            continue;
        if (v.children.empty())
            continue;
        auto fn_id = v.child(0);
        if (fn_id == aura::ast::NULL_NODE)
            continue;
        auto fn = flat.get(fn_id);
        if (fn.tag != aura::ast::NodeTag::Variable)
            continue;
        // We only flag the call site; the caller's name
        // (mutate:*, etc.) is checked at the call site in
        // lower_to_ir_impl's inference loop via the pool.
        out.insert(i);
    }
}

// Issue #896: cache Call-site set keyed by FlatAST address + generation.
// Invalidated when FlatAST generation bumps (structural mutation).
static const std::unordered_set<aura::ast::NodeId>& mutation_calls_cached(const FlatAST& flat) {
    struct Cache {
        const FlatAST* flat = nullptr;
        std::uint16_t gen = 0;
        std::unordered_set<aura::ast::NodeId> nodes;
    };
    static thread_local Cache cache;
    const auto gen = flat.generation();
    if (cache.flat == &flat && cache.gen == gen)
        return cache.nodes;
    cache.nodes.clear();
    collect_mutation_calls(flat, cache.nodes);
    cache.flat = &flat;
    cache.gen = gen;
    return cache.nodes;
}

// Issue #896: memoized subtree reachability — O(N) total instead of
// O(M·N·depth) repeated recursive walks. memo[id]: -1 unknown, 0/1 result.
static bool subtree_uses_node_memo(const FlatAST& flat, aura::ast::NodeId root,
                                   const std::unordered_set<aura::ast::NodeId>& target_set,
                                   std::vector<std::int8_t>& memo) {
    if (root == aura::ast::NULL_NODE || root >= flat.size())
        return false;
    if (memo[root] >= 0)
        return memo[root] != 0;
    if (target_set.count(root)) {
        memo[root] = 1;
        return true;
    }
    auto v = flat.get(root);
    for (auto c : v.children) {
        if (subtree_uses_node_memo(flat, c, target_set, memo)) {
            memo[root] = 1;
            return true;
        }
    }
    memo[root] = 0;
    return false;
}

// ── Internal: lower_to_ir with optional cache ──────────────────
static IRModule lower_to_ir_impl(
    FlatAST& flat, StringPool& pool, ASTArena& arena,
    const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>* cache,
    std::vector<std::string>* cache_hits = nullptr, const Primitives* primitives = nullptr,
    const aura::core::TypeRegistry* type_reg = nullptr,
    const std::unordered_map<std::string, std::vector<aura::ir::ClosureBridgeData>>* cache_bridge =
        nullptr,
    const std::unordered_map<std::string, std::vector<std::string>>* cache_strings = nullptr,
    const std::string* self_name = nullptr,
    const std::unordered_map<std::string, std::size_t>* value_cells = nullptr,
    std::uint32_t narrowing_evidence = 0) { // Issue #280
    LoweringState state(arena);
    // Issue #684: production dual-emit to IRFunctionSoA columns.
    state.enable_soa_dual_emit();
    // Issue #1318 Phase 1: track dual-emit bridge usage toward full SoA primary path.
    aura::compiler::ir_soa_migration::record_dual_emit_bridge();
    state.instruction_reserve_hint = flat.size();
    state.value_cells = value_cells;
    state.current_narrowing_evidence = narrowing_evidence;

    // Issue #150 Phase 2: automatic region inference pass.
    // Walk the FlatAST before main lowering. For each Define
    // and Lambda, recursively scan the body for mutation
    // primitives (mutate:*, eval-current, ast:*, etc.). If
    // any are found and the function has no explicit
    // annotation, set the region to Evolution. Don't
    // override explicit (performance-region / evolution-
    // region) annotations — those take precedence.
    //
    // Conservative MVP: only direct mutation detection in
    // the function body. Transitive inference (a function
    // that calls another function marked Evolution) is
    // Phase 2b/3 (call graph + mutation impact analysis).
    {
        // Issue #896: generation-cached Call set + shared memo for all
        // Define/Lambda probes (O(N) total, reuses cache across re-lowers).
        const auto& mutating_nodes = mutation_calls_cached(flat);
        std::vector<std::int8_t> mut_memo(flat.size(), static_cast<std::int8_t>(-1));
        for (aura::ast::NodeId i = 0; i < flat.size(); ++i) {
            if (i >= flat.size())
                break;
            auto v = flat.get(i);
            if (v.tag == aura::ast::NodeTag::Define) {
                auto sym = v.sym_id;
                if (sym == aura::ast::INVALID_SYM)
                    continue;
                if (flat.get_function_region_for_sym(sym).has_value())
                    continue;
                if (subtree_uses_node_memo(flat, v.child(0), mutating_nodes, mut_memo)) {
                    flat.set_function_region(
                        sym, static_cast<std::uint8_t>(aura::ir::Region::Evolution));
                }
            } else if (v.tag == aura::ast::NodeTag::Lambda) {
                if (flat.get_function_region_for_lambda(i).has_value())
                    continue;
                if (subtree_uses_node_memo(flat, i, mutating_nodes, mut_memo)) {
                    flat.set_function_region_lambda(
                        i, static_cast<std::uint8_t>(aura::ir::Region::Evolution));
                }
            }
        }
    }
    state.primitives = primitives;
    state.type_reg = type_reg;
    state.cache_bridge = cache_bridge;
    state.cache_strings = cache_strings;
    if (self_name && !self_name->empty()) {
        state.self_name = *self_name;
    }
    state.module = {};
    // Create top-level function
    IRFunction top_func;
    top_func.id = 0;
    top_func.name = "__top__";
    top_func.entry_block = 0;
    top_func.blocks.push_back({0, {}, {}});
    state.cur_func = &top_func;
    state.cur_block = 0;
    state.local_count = 0;
    // Issue #684: bootstrap SoA __top__ function + entry block (the AoS
    // path creates the first block directly; dual-emit needs parity).
    if (state.dual_emit_soa) {
        state.module_v2.functions.push_back({});
        if (state.instruction_reserve_hint > 0)
            state.module_v2.functions.back().reserve(state.instruction_reserve_hint);
        state.cur_func_v2_idx = 0;
        (void)state.module_v2.add_block(state.cur_func_v2_idx);
        ++state.soa_functions_emitted;
    }

    auto result_slot = lower_flat_expr(state, flat, pool, flat.root, cache, cache_hits);

    // Emit return
    state.emit(IROpcode::Return, result_slot);
    top_func.local_count = state.local_count;
    top_func.region = state.region;
    auto top_id = state.module.add_function(std::move(top_func));
    state.module.entry_function_id = top_id;
    // Issue #684: seal the final SoA block + publish snapshot.
    if (state.dual_emit_soa && !state.module_v2.functions.empty()) {
        const auto v2_idx = state.cur_func_v2_idx;
        auto& soa_fn = state.module_v2.functions[v2_idx];
        if (!soa_fn.blocks_.empty()) {
            state.module_v2.seal_block(v2_idx,
                                       static_cast<std::uint32_t>(soa_fn.blocks_.size() - 1));
        }
        soa_fn.local_count = state.local_count;
        if (!top_func.name.empty())
            soa_fn.name = top_func.name;
    }
    g_last_soa_snapshot.instructions_emitted = state.soa_instructions_emitted;
    g_last_soa_snapshot.functions_emitted = state.soa_functions_emitted;
    g_last_soa_snapshot.type_metadata_stamped = state.soa_type_metadata_stamped;
    g_last_soa_snapshot.module = std::move(state.module_v2);
    // Issue #1258: post dual-emit strong consistency check —
    // SoA function/instr counts must match AoS module shape when
    // dual-emit was active. Mismatch bumps a counter and forces
    // full dirty on the snapshot module (safe re-lower fallback).
    if (state.dual_emit_soa) {
        const auto aos_fns = state.module.functions.size();
        const auto soa_fns = g_last_soa_snapshot.module.functions.size();
        const bool ok = (soa_fns == aos_fns) || (aos_fns == 0);
        g_last_soa_snapshot.consistency_ok = ok;
        ++g_last_soa_snapshot.consistency_checks;
        if (!ok) {
            ++g_last_soa_snapshot.consistency_mismatches;
            for (auto& fn : g_last_soa_snapshot.module.functions)
                fn.mark_all_blocks_dirty();
        }
    }
    return state.module;
}

// ── lower_to_ir (FlatAST path) — native, no full-tree reconstruct ──
IRModule lower_to_ir(FlatAST& flat, StringPool& pool, ASTArena& arena, const Primitives* primitives,
                     const aura::core::TypeRegistry* type_reg,
                     std::uint32_t narrowing_evidence) { // Issue #280
    // Issue #127: delegate to the Result-returning version.
    // Lowering never reports an error today (it's all
    // fall-through: silently emits a ConstI64 0 for unknown
    // refs), so unwrap() is safe. When the lowering path
    // starts producing diagnostics (e.g., for type-registry
    // lookups), the unwrap becomes a real error channel.
    return lower_to_ir_result(flat, pool, arena, primitives, type_reg, narrowing_evidence).value();
}

// ── lower_to_ir_with_cache ─────────────────────────────────────
IRModule lower_to_ir_with_cache(
    FlatAST& flat, StringPool& pool, ASTArena& arena,
    const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>* cache,
    std::vector<std::string>* cache_hits, const Primitives* primitives,
    const std::unordered_map<std::string, std::vector<aura::ir::ClosureBridgeData>>* cache_bridge,
    const std::unordered_map<std::string, std::vector<std::string>>* cache_strings,
    const std::string* self_name, const aura::core::TypeRegistry* type_reg,
    const std::unordered_map<std::string, std::size_t>* value_cells,
    std::uint32_t narrowing_evidence) { // Issue #280
    return lower_to_ir_with_cache_result(flat, pool, arena, cache, cache_hits, primitives,
                                         cache_bridge, cache_strings, self_name, type_reg,
                                         value_cells, narrowing_evidence)
        .value();
}

// ── lower_to_ir_result / lower_to_ir_with_cache_result ─────────
//
// Issue #127: Result-returning variants. The implementation
// calls the same private `lower_to_ir_impl` and wraps the
// result. Today, `lower_to_ir_impl` never fails (it's a
// fall-through path), so the result is always `Ok`.
//
// When the lowering path starts producing real diagnostics
// (e.g., for unknown primitives, type-registry lookups, or
// invalid IR shapes), this is the channel through which
// those errors flow to the caller.
aura::diag::LowerResult<IRModule> lower_to_ir_result(FlatAST& flat, StringPool& pool,
                                                     ASTArena& arena, const Primitives* primitives,
                                                     const aura::core::TypeRegistry* type_reg,
                                                     std::uint32_t narrowing_evidence) { // #280
    return lower_to_ir_impl(flat, pool, arena, nullptr, nullptr, primitives, type_reg, nullptr,
                            nullptr, nullptr, nullptr, narrowing_evidence);
}

aura::diag::LowerResult<IRModule> lower_to_ir_with_cache_result(
    FlatAST& flat, StringPool& pool, ASTArena& arena,
    const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>* cache,
    std::vector<std::string>* cache_hits, const Primitives* primitives,
    const std::unordered_map<std::string, std::vector<aura::ir::ClosureBridgeData>>* cache_bridge,
    const std::unordered_map<std::string, std::vector<std::string>>* cache_strings,
    const std::string* self_name, const aura::core::TypeRegistry* type_reg,
    const std::unordered_map<std::string, std::size_t>* value_cells,
    std::uint32_t narrowing_evidence) { // Issue #280
    return lower_to_ir_impl(flat, pool, arena, cache, cache_hits, primitives, type_reg,
                            cache_bridge, cache_strings, self_name, value_cells,
                            narrowing_evidence);
}

// ── lower_function_at — per-function lowering (Issue #224 cycle 3) ─
//
// Lower a single Lambda AST node into a self-contained
// IRFunction. Reuses lower_to_ir_impl to do the work, then
// extracts the Lambda function (the one that isn't the
// __top__ entry function).
//
// Cycle 3 scope notes:
//   - The __top__ entry function is created by lower_to_ir_impl
//     and then discarded. The call is wasteful but simple.
//   - Re-running the region inference pass + caching on the
//     caller's flat is OK because the inference is idempotent
//     (marks functions with mutation calls as Evolution; safe
//     to re-run).
//   - The returned function's id is 0; the caller is
//     responsible for assigning the correct func_id and
//     rebinding MakeClosure operands in callers.
//   - The per-function passes (compute_kind, constant_fold) are
//     NOT run here; the caller should run them after
//     replacement, matching what cache_define does for the
//     full-bundle path.
IRFunction
lower_function_at(FlatAST& flat, StringPool& pool, ASTArena& arena, NodeId lambda_node_id,
                  const Primitives* primitives,
                  const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>* cache,
                  std::vector<std::string>* cache_hits) {
    if (lambda_node_id == NULL_NODE || lambda_node_id >= flat.size()) {
        return IRFunction{};
    }
    // Save the current root and replace with the Lambda node.
    // lower_to_ir_impl starts from flat.root, so swapping
    // root lets the same pipeline lower just the Lambda
    // subtree. We restore the root on the way out.
    auto saved_root = flat.root;
    flat.root = lambda_node_id;
    IRModule ir_mod;
    // Use the simpler lower_to_ir (no cache) — the per-function
    // path doesn't need cache_hits / dep tracking (the caller
    // already has the dependency graph and the function is
    // self-contained).
    (void)cache;
    (void)cache_hits;
    ir_mod = lower_to_ir_impl(flat, pool, arena, /*cache=*/nullptr, /*cache_hits=*/nullptr,
                              primitives, /*type_reg=*/nullptr,
                              /*cache_bridge=*/nullptr, /*cache_strings=*/nullptr,
                              /*self_name=*/nullptr);
    flat.root = saved_root;
    // The result has functions: [0] = __top__ (entry, with
    // MakeClosure call), [1] = __lambda__ (the actual function).
    // We return [1] and let the caller re-assign func_id.
    if (ir_mod.functions.size() < 2) {
        return IRFunction{};
    }
    // Find the non-entry function (in case the lowering added
    // helper functions for nested lambdas; for the simple
    // case there's exactly one).
    // For cycle 3 we return the FIRST non-entry function and
    // discard any nested lambdas (those are re-lowered on
    // subsequent per-function calls or the next full re-lower).
    for (auto& fn : ir_mod.functions) {
        if (fn.id != ir_mod.entry_function_id) {
            fn.id = 0; // caller assigns
            return std::move(fn);
        }
    }
    return IRFunction{};
}

// ── unparse_node — FlatAST → S-expression source ───────────────
std::string unparse_node(const FlatAST& flat, const StringPool& pool, NodeId id, int indent) {
    if (id == NULL_NODE || id >= flat.size())
        return "()";
    auto v = flat.get(id);
    auto indent_str = [](int d) { return std::string(static_cast<std::size_t>(d * 2), ' '); };

    switch (v.tag) {
        case NodeTag::LiteralInt:
            return std::to_string(v.int_value);

        case NodeTag::LiteralString: {
            auto s = pool.resolve(v.sym_id);
            std::string out = "\"";
            for (char c : s) {
                if (c == '"')
                    out += "\\\"";
                else if (c == '\\')
                    out += "\\\\";
                else if (c == '\n')
                    out += "\\n";
                else
                    out += c;
            }
            return out + "\"";
        }

        case NodeTag::Variable:
            return std::string(pool.resolve(v.sym_id));

        case NodeTag::Call: {
            // Check: is this a lambda application? ((lambda ...) ...)
            // Format: (func arg1 arg2 ...)
            auto callee = v.child(0);
            std::string s = "(";
            s += unparse_node(flat, pool, callee, indent + 1);

            for (std::size_t i = 1; i < v.children.size(); ++i) {
                auto arg_str = unparse_node(flat, pool, v.child(i), indent + 1);
                // If arg is multiline or makes total line too long, use newline
                bool long_arg =
                    arg_str.size() > 40 || (s.size() + arg_str.size() + 2 > 80 && s.size() > 20);
                if (long_arg) {
                    s += "\n" + indent_str(indent + 1) + arg_str;
                } else {
                    s += " " + arg_str;
                }
            }
            s += ")";
            return s;
        }

        case NodeTag::IfExpr: {
            std::string s = "(if ";
            s += unparse_node(flat, pool, v.child(0), indent + 1);
            auto then_str = unparse_node(flat, pool, v.child(1), indent + 1);
            auto else_str = unparse_node(flat, pool, v.child(2), indent + 1);
            if (then_str.size() + else_str.size() > 40) {
                s += "\n" + indent_str(indent + 1) + then_str;
                s += "\n" + indent_str(indent + 1) + else_str;
            } else {
                s += " " + then_str + " " + else_str;
            }
            s += ")";
            return s;
        }

        case NodeTag::Lambda: {
            std::string s = "(lambda (";
            for (std::size_t i = 0; i < v.params.size(); ++i) {
                if (i > 0)
                    s += " ";
                s += pool.resolve(v.params[i]);
            }
            s += ")\n" + indent_str(indent + 1);
            s += unparse_node(flat, pool, v.child(0), indent + 1);
            s += ")";
            return s;
        }

        case NodeTag::Let:
        case NodeTag::LetRec: {
            std::string kw = (v.tag == NodeTag::LetRec) ? "letrec" : "let";
            std::string s = "(" + kw + " ((";
            s += pool.resolve(v.sym_id);
            s += " ";
            s += unparse_node(flat, pool, v.child(0), indent + 1);
            s += "))\n" + indent_str(indent + 1);
            s += unparse_node(flat, pool, v.child(1), indent + 1);
            s += ")";
            return s;
        }

        case NodeTag::Define: {
            std::string s = "(define ";
            s += pool.resolve(v.sym_id);
            auto val_str = unparse_node(flat, pool, v.child(0), indent + 1);
            if (val_str.size() > 30) {
                s += "\n" + indent_str(indent + 1) + val_str;
            } else {
                s += " " + val_str;
            }
            s += ")";
            return s;
        }

        case NodeTag::Begin: {
            std::string s = "(begin";
            for (std::size_t i = 0; i < v.children.size(); ++i) {
                s += "\n" + indent_str(indent + 1);
                s += unparse_node(flat, pool, v.child(i), indent + 1);
            }
            return s + ")";
        }

        case NodeTag::Set: {
            return "(set! " + std::string(pool.resolve(v.sym_id)) + " " +
                   unparse_node(flat, pool, v.child(0), indent + 1) + ")";
        }

        case NodeTag::Pair: {
            return "(" + unparse_node(flat, pool, v.child(0), indent + 1) + " . " +
                   unparse_node(flat, pool, v.child(1), indent + 1) + ")";
        }

        case NodeTag::Quote: {
            return "(quote " + unparse_node(flat, pool, v.child(0), indent + 1) + ")";
        }

        case NodeTag::TypeAnnotation: {
            return "(the " + std::string(pool.resolve(v.sym_id)) + " " +
                   unparse_node(flat, pool, v.child(0), indent + 1) + ")";
        }

        case NodeTag::Coercion: {
            return "(coerce " + unparse_node(flat, pool, v.child(0), indent + 1) + ")";
        }

        case NodeTag::MacroDef: {
            std::string s = "(defmacro (";
            s += pool.resolve(v.sym_id);
            for (std::size_t i = 0; i < v.params.size(); ++i) {
                s += " ";
                s += pool.resolve(v.params[i]);
            }
            s += ")\n" + indent_str(indent + 1);
            if (!v.children.empty())
                s += unparse_node(flat, pool, v.child(0), indent + 1);
            s += ")";
            return s;
        }

        default:
            return "()";
    }
}

} // namespace aura::compiler
