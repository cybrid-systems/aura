# 0 "/home/dev/code/aura/src/compiler/lowering_impl.cpp"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/compiler/lowering_impl.cpp"
module aura.compiler.lowering;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler {

using namespace aura::ir;
using namespace aura::ast;
# 19 "/home/dev/code/aura/src/compiler/lowering_impl.cpp"
static void remap_func_ids(aura::ir::IRFunction& func, std::uint32_t base_fid) {
    for (auto& block : func.blocks) {
        for (auto& inst : block.instructions) {
            if (inst.opcode == aura::ir::IROpcode::MakeClosure) {





                inst.operands[1] += base_fid - 1;
            }
        }
    }
}

static std::uint32_t lower_flat_expr(LoweringState& state,
                                      const FlatAST& flat,
                                      StringPool& pool,
                                      NodeId id,
                                      const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>* cache = nullptr,
                                      std::vector<std::string>* cache_hits = nullptr) {

    state.current_flat = &flat;
    state.current_pool = &pool;
    state.current_source_id = id;

    if (id == NULL_NODE || id >= flat.size()) {
        auto slot = state.alloc_local();
        state.emit(IROpcode::ConstI64, slot, 0, 0);
        return slot;
    }
    auto v = flat.get(id);

    switch (v.tag) {
    case NodeTag::LiteralFloat: {
        auto slot = state.alloc_local();
        std::uint64_t bits;
        std::memcpy(&bits, &v.float_value, sizeof(bits));
        state.emit(IROpcode::ConstF64, slot, static_cast<std::uint32_t>(bits & 0xFFFFFFFF), static_cast<std::uint32_t>(bits >> 32));
        return slot;
    }
    case NodeTag::LiteralInt: {
        auto slot = state.alloc_local();
        auto marker = flat.marker(id);
        if (marker == aura::ast::SyntaxMarker::BoolLiteral) {

            state.emit(IROpcode::ConstBool, slot, v.int_value != 0 ? 1 : 0);
        } else {
            auto val = static_cast<std::uint64_t>(v.int_value);
            state.emit(IROpcode::ConstI64, slot,
                 static_cast<std::uint32_t>(val),
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

        for (auto it = state.scopes.rbegin(); it != state.scopes.rend(); ++it) {
            auto found = it->find(std::string(name));
            if (found != it->end()) {
                auto& binding = found->second;
                auto slot = state.alloc_local();
                switch (binding.kind) {
                case BindingKind::Local:
                    state.emit(IROpcode::Local, slot, binding.slot); break;
                case BindingKind::Captured:
                    state.emit(IROpcode::Local, slot, binding.slot); break;
                case BindingKind::Cell:
                    state.emit(IROpcode::CellGet, slot, binding.slot); break;
                }
                return slot;
            }
        }

        auto fv = state.free_var_map.find(std::string(name));
        if (fv != state.free_var_map.end()) {
            auto slot = state.alloc_local();
            state.emit(IROpcode::Local, slot, fv->second);
            return slot;
        }

        if (cache) {
            auto cache_it = cache->find(std::string(name));
            if (cache_it != cache->end()) {
                if (cache_hits) {
                    cache_hits->push_back(std::string(name));
                }

                auto base_fid = static_cast<std::uint32_t>(state.module.functions.size());

                std::uint32_t lambda_fid = 0;
                for (std::size_t ci = 0; ci < cache_it->second.size(); ++ci) {
                    auto& func = cache_it->second[ci];
                    auto copy = func;
                    remap_func_ids(copy, base_fid);

                    if (state.cache_strings) {
                        auto str_it = state.cache_strings->find(std::string(name));
                        if (str_it != state.cache_strings->end() && !str_it->second.empty()) {
                            auto& str_pool = str_it->second;
                            for (auto& blk : copy.blocks)
                                for (auto& inst : blk.instructions)
                                    if (inst.opcode == IROpcode::ConstString &&
                                        inst.operands[1] < str_pool.size())
                                        inst.operands[1] = state.module.add_string(str_pool[inst.operands[1]]);
                        }
                    }
                    auto new_fid = state.module.add_function(std::move(copy));


                    if (ci == 0) lambda_fid = new_fid;

                    if (state.cache_bridge) {
                        auto bridge_it = state.cache_bridge->find(std::string(name));
                        if (bridge_it != state.cache_bridge->end() && ci < bridge_it->second.size()) {
                            state.module.set_closure_bridge_ptr(new_fid,
                                bridge_it->second[ci].flat,
                                bridge_it->second[ci].pool,
                                bridge_it->second[ci].body_id);

                            if (!bridge_it->second[ci].body_source.empty() &&
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

        if (state.primitives) {
            auto slot_idx = state.primitives->slot_for_name(std::string(name));
            if (slot_idx < state.primitives->slot_count()) {
                auto slot = state.alloc_local();
                state.emit(IROpcode::Primitive, slot, static_cast<std::uint32_t>(slot_idx));
                return slot;
            }
        }



        if (state.self_func_id != 0 && std::string(name) == state.self_name) {
            auto slot = state.alloc_local();
            state.emit(IROpcode::MakeClosure, slot, state.self_func_id, 0);
            return slot;
        }
        auto slot = state.alloc_local();
        state.emit(IROpcode::ConstI64, slot, 0, 0);
        return slot;
    }
    case NodeTag::Call: {
        auto callee_v = flat.get(v.child(0));



        if (callee_v.tag == NodeTag::Variable) {
            auto callee_name = pool.resolve(callee_v.sym_id);
            static const std::unordered_map<std::string, IROpcode> prim_map = {
                {"+", IROpcode::Add}, {"-", IROpcode::Sub},
                {"*", IROpcode::Mul}, {"/", IROpcode::Div},
                {"=", IROpcode::Eq}, {"<", IROpcode::Lt},
                {">", IROpcode::Gt}, {"<=", IROpcode::Le},
                {">=", IROpcode::Ge},
                {"eq?", IROpcode::Eq},
                {"eqv?", IROpcode::Eq},
                {"equal?", IROpcode::Eq},
                {"cons", IROpcode::MakePair},
                {"car", IROpcode::Car},
                {"cdr", IROpcode::Cdr},
            };
            auto it = prim_map.find(std::string(callee_name));
            if (it != prim_map.end()) {
                auto op = it->second;
                auto result_slot = state.alloc_local();
                auto arg_count = v.children.size() - 1;
                if (arg_count == 0) {

                    state.emit(IROpcode::ConstI64, result_slot, 0, 0);
                } else {

                    auto prev = lower_flat_expr(state, flat, pool, v.child(1), cache, cache_hits);
                    if (arg_count == 1) {

                        if (std::string(callee_name) == "-") {
                            auto zero = state.alloc_local();
                            state.emit(IROpcode::ConstI64, zero, 0, 0);
                            state.emit(op, result_slot, zero, prev);
                        } else {
                            state.emit(op, result_slot, prev, prev);
                        }
                    } else if (arg_count == 2) {
                        auto arg1 = lower_flat_expr(state, flat, pool, v.child(2), cache, cache_hits);
                        state.emit(op, result_slot, prev, arg1);
                    } else {


                        bool is_comp = (op == IROpcode::Eq || op == IROpcode::Lt ||
                                        op == IROpcode::Gt || op == IROpcode::Le ||
                                        op == IROpcode::Ge);
                        if (is_comp) {

                            bool first = true;
                            auto pair_prev = prev;
                            auto and_acc = state.alloc_local();
                            for (std::size_t ai = 2; ai < v.children.size(); ++ai) {
                                auto pair_next = lower_flat_expr(state, flat, pool, v.child(ai), cache, cache_hits);
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

                            auto arg1 = lower_flat_expr(state, flat, pool, v.child(2), cache, cache_hits);
                            auto acc = state.alloc_local();
                            state.emit(op, acc, prev, arg1);
                            for (std::size_t ai = 3; ai < v.children.size(); ++ai) {
                                auto next = lower_flat_expr(state, flat, pool, v.child(ai), cache, cache_hits);
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


            if (std::string(callee_name) == "try") {

                if (v.children.size() < 2) {
                    auto slot = state.alloc_local();
                    state.emit(IROpcode::ConstI64, slot, 0, 0);
                    return slot;
                }
                auto body_slot = lower_flat_expr(state, flat, pool, v.child(1), cache, cache_hits);
                auto is_err_slot = state.alloc_local();
                state.emit(IROpcode::IsError, is_err_slot, body_slot);

                auto handler_blk = state.alloc_block();
                auto continue_blk = state.alloc_block();
                auto end_blk = state.alloc_block();
                state.emit(IROpcode::Branch, is_err_slot, handler_blk, continue_blk);


                state.cur_block = continue_blk;
                auto phi_slot = state.alloc_local();
                state.emit(IROpcode::Local, phi_slot, body_slot);
                state.emit(IROpcode::Jump, end_blk);


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

                                if (catch_v.children.size() >= 3) {
                                    auto var_form = flat.get(catch_v.child(1));
                                    std::string var_name;
                                    if (var_form.tag == NodeTag::Call && var_form.children.size() >= 1) {
                                        auto var_node = flat.get(var_form.child(0));
                                        if (var_node.tag == NodeTag::Variable)
                                            var_name = std::string(pool.resolve(var_node.sym_id));
                                    }

                                    auto err_slot = state.alloc_local();
                                    state.emit(IROpcode::Local, err_slot, body_slot);

                                    if (state.scopes.empty()) {
                                        state.scopes.push_back({});
                                    }
                                    if (!var_name.empty()) {
                                        state.scopes.back()[var_name] = Binding{BindingKind::Local, err_slot};
                                    }
                                    auto handler_slot = lower_flat_expr(state, flat, pool, catch_v.child(2), cache, cache_hits);
                                    state.emit(IROpcode::Local, phi_slot, handler_slot);
                                    caught = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (!caught) {

                    state.emit(IROpcode::Local, phi_slot, body_slot);
                }
                state.emit(IROpcode::Jump, end_blk);

                state.cur_block = end_blk;
                return phi_slot;
            }


            if (std::string(callee_name) == "require") {
                auto slot = state.alloc_local();
                state.emit(IROpcode::ConstVoid, slot);
                return slot;
            }


            static const std::unordered_map<std::string, PrimId> prim_call_map = {
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
            };
            auto pcit = prim_call_map.find(std::string(callee_name));
            if (pcit != prim_call_map.end()) {
                auto prim_id = static_cast<std::uint32_t>(pcit->second);
                auto arg_count = static_cast<std::uint32_t>(v.children.size() - 1);
                auto arg_base = state.local_count;
                for (std::size_t i = 1; i < v.children.size(); ++i) {
                    auto val_slot = lower_flat_expr(state, flat, pool, v.child(i), cache, cache_hits);
                    state.emit(IROpcode::Local, arg_base + static_cast<std::uint32_t>(i - 1), val_slot);
                    state.alloc_local();
                }
                auto result_slot = state.alloc_local();
                state.emit(IROpcode::PrimCall, prim_id,
                           pack_pair(arg_base, arg_count), result_slot, 0);
                return result_slot;
            }



        }


        auto callee_slot = lower_flat_expr(state, flat, pool, v.child(0), cache, cache_hits);
        auto arg_count = static_cast<std::uint32_t>(v.children.size() - 1);

        auto arg_base = state.local_count;
        for (std::size_t i = 1; i < v.children.size(); ++i) {
            auto val_slot = lower_flat_expr(state, flat, pool, v.child(i), cache, cache_hits);
            state.emit(IROpcode::Local, arg_base + static_cast<std::uint32_t>(i - 1), val_slot);
            state.alloc_local();
        }
        auto result_slot = state.alloc_local();

        state.emit(IROpcode::Call, callee_slot, arg_base, arg_count, result_slot);
        return result_slot;
    }
    case NodeTag::IfExpr: {
        auto cond_slot = lower_flat_expr(state, flat, pool, v.child(0), cache, cache_hits);
        auto then_blk = state.alloc_block();
        auto else_blk = state.alloc_block();
        auto merge_blk = state.alloc_block();
        state.emit(IROpcode::Branch, cond_slot, then_blk, else_blk);

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


        auto param_span = v.params;
        std::vector<std::string> param_names;
        for (auto pid : param_span)
            param_names.push_back(std::string(pool.resolve(pid)));


        std::unordered_set<std::string> free_set;
        std::unordered_set<std::string> bound(param_names.begin(), param_names.end());

        {
            struct FreeVarWalker {
                const FlatAST& flat;
                StringPool& pool;
                std::unordered_set<std::string>& free;
                std::unordered_set<std::string>& bound;
                void walk(NodeId nid) {
                    if (nid == NULL_NODE || nid >= flat.size()) return;
                    auto nv = flat.get(nid);
                    switch (nv.tag) {
                    case NodeTag::Variable: {
                        auto name = pool.resolve(nv.sym_id);
                        if (bound.find(std::string(name)) == bound.end())
                            free.insert(std::string(name));
                        break;
                    }
                    case NodeTag::LiteralInt: case NodeTag::LiteralString: break;
                    default:
                        for (auto c : nv.children) walk(c);
                        break;
                    }
                }
            };
            FreeVarWalker{flat, pool, free_set, bound}.walk(v.child(0));
        }


        std::vector<std::string> free_vars;
        for (auto& fv : free_set) {
            bool in_scope = false;
            for (auto it = state.scopes.rbegin(); it != state.scopes.rend(); ++it) {
                if (it->find(fv) != it->end()) { in_scope = true; break; }
            }
            if (in_scope) free_vars.push_back(fv);
        }


        IRFunction func;
        func.name = "__lambda__";
        func.entry_block = 0;
        func.blocks.push_back({0, {}, {}});
        func.params = param_names;
        func.arg_count = static_cast<std::uint32_t>(param_names.size());
        func.variadic = (v.int_value != 0);


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

        state.env_slot = state.alloc_local();
        state.scopes.push_back({});


        for (std::size_t i = 0; i < free_vars.size(); ++i) {
            auto& fv = free_vars[i];
            auto slot = state.alloc_local();
            state.emit(IROpcode::Arg, slot, static_cast<std::uint32_t>(i));

            bool is_cell = false;
            for (auto it = saved_scopes.rbegin(); it != saved_scopes.rend(); ++it) {
                auto found = it->find(fv);
                if (found != it->end() && found->second.kind == BindingKind::Cell) {
                    is_cell = true; break;
                }
            }
            if (is_cell) {





                state.scopes.back()[fv] = Binding{BindingKind::Cell, slot};
                state.free_var_map[fv] = static_cast<std::uint32_t>(i);
                state.cell_free_vars.insert(fv);
            } else {
                state.scopes.back()[fv] = Binding{BindingKind::Captured, slot};
                state.free_var_map[fv] = static_cast<std::uint32_t>(i);
            }
        }


        for (std::size_t i = 0; i < param_names.size(); ++i) {
            auto slot = state.alloc_local();
            state.emit(IROpcode::Arg, slot,
                 static_cast<std::uint32_t>(free_vars.size() + i));
            state.scopes.back()[param_names[i]] = Binding{BindingKind::Local, slot};
        }


        auto body_slot = lower_flat_expr(state, flat, pool, v.child(0), cache, cache_hits);
        state.emit(IROpcode::Return, body_slot);

        func.local_count = state.local_count;


        state.cur_func = saved_func;
        state.cur_block = saved_block;
        state.local_count = saved_locals;
        state.scopes = std::move(saved_scopes);
        state.env_slot = saved_env_slot;
        state.free_var_map = std::move(saved_fv_map);
        state.self_func_id = saved_self_func_id;

        func.free_vars = free_vars;
        if (!state.current_flat) std::println("DEBUG: current_flat NULL at lambda bridge data");
        if (!state.current_pool) std::println("DEBUG: current_pool NULL at lambda bridge data");
        auto fid = state.module.add_function(std::move(func));

        if (state.current_flat && state.current_pool) {
            state.module.set_closure_bridge(fid, state.current_flat, state.current_pool, v.child(0));

            auto body_src = unparse_node(*state.current_flat, *state.current_pool, v.child(0));
            state.module.set_closure_body_source(fid, body_src);
        }
        auto slot = state.alloc_local();
        state.emit(IROpcode::MakeClosure, slot, fid,
             static_cast<std::uint32_t>(free_vars.size()));




        for (std::size_t i = 0; i < free_vars.size(); ++i) {
            auto& fv = free_vars[i];
            for (auto it = state.scopes.rbegin(); it != state.scopes.rend(); ++it) {
                auto found = it->find(fv);
                if (found != it->end()) {
                    auto& binding = found->second;
                    auto cslot = state.alloc_local();
                    state.emit(IROpcode::Local, cslot, binding.slot);
                    state.emit(IROpcode::Capture, slot,
                         static_cast<std::uint32_t>(i), cslot);
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
        auto& scope = state.scopes.empty()
            ? (state.scopes.push_back({}), state.scopes.back())
            : state.scopes.back();


        auto existing = scope.find(std::string(name));
        bool has_cell = (existing != scope.end() && existing->second.kind == BindingKind::Cell);
        std::uint32_t ci;
        if (has_cell) {
            ci = existing->second.slot;
        } else {


            ci = state.alloc_local();
            state.emit(IROpcode::NewCell, ci);
            scope[std::string(name)] = Binding{BindingKind::Cell, ci};
        }

        auto val_slot = lower_flat_expr(state, flat, pool, val_id, cache, cache_hits);
        state.emit(IROpcode::CellSet, ci, val_slot);
        return val_slot;
    }
    case NodeTag::Begin: {


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

        std::uint32_t last_slot = 0;
        for (auto c : v.children)
            last_slot = lower_flat_expr(state, flat, pool, c, cache, cache_hits);
        if (!v.children.empty()) return last_slot;
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

                state.emit(IROpcode::Local, binding.slot, val_slot);
                return val_slot;
            }
        }
        return val_slot;
    }
    case NodeTag::Quote: {

        if (!v.children.empty()) {
            auto cv = flat.get(v.child(0));
            if (cv.tag == NodeTag::LiteralInt) {
                if (cv.int_value == 0) {

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




        std::function<std::uint32_t(NodeId)> lower_q;
        lower_q = [&](NodeId nid) -> std::uint32_t {
            if (nid == ast::NULL_NODE || nid >= flat.size()) {
                auto s = state.alloc_local();
                state.emit(IROpcode::ConstI64, s, 0, 0);
                return s;
            }
            auto nv = flat.get(nid);

            if (nv.tag == NodeTag::LiteralInt) {
                auto s = state.alloc_local();
                state.emit(IROpcode::ConstI64, s, nv.int_value, 0);
                return s;
            }
            if (nv.tag == NodeTag::LiteralFloat || nv.tag == NodeTag::LiteralString) {
                return lower_flat_expr(state, flat, pool, nid, cache, cache_hits);
            }

            if (nv.tag == NodeTag::Pair) {

                auto left = nv.children.empty() ? ast::NULL_NODE : nv.child(0);
                auto right = nv.children.size() > 1 ? nv.child(1) : ast::NULL_NODE;
                auto left_slot = lower_q(left);
                auto right_slot = lower_q(right);

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

            return lower_flat_expr(state, flat, pool, nid, cache, cache_hits);
        };

        return lower_q(v.child(0));
    }
    case NodeTag::TypeAnnotation:
        return lower_flat_expr(state, flat, pool, v.child(0), cache, cache_hits);
    case NodeTag::Coercion: {
        auto inner = lower_flat_expr(state, flat, pool, v.child(0), cache, cache_hits);
        auto slot = state.alloc_local();

        std::uint32_t type_tag = static_cast<std::uint32_t>(v.int_value);

        std::uint32_t blame_loc = (static_cast<std::uint32_t>(v.line) << 16) |
                                   (static_cast<std::uint32_t>(v.col) & 0xFFFFu);
        state.emit(IROpcode::CastOp, slot, inner, type_tag, blame_loc);
        return slot;
    }
    case NodeTag::MacroDef:
    default: {

        auto slot = state.alloc_local();
        state.emit(IROpcode::ConstI64, slot, 0, 0);
        return slot;
    }
    }
}


static IRModule lower_to_ir_impl(FlatAST& flat, StringPool& pool, ASTArena& arena,
                                  const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>* cache,
                                  std::vector<std::string>* cache_hits = nullptr,
                                  const Primitives* primitives = nullptr,
                                  const std::unordered_map<std::string, std::vector<aura::ir::ClosureBridgeData>>* cache_bridge = nullptr,
                                  const std::unordered_map<std::string, std::vector<std::string>>* cache_strings = nullptr,
                                  const std::string* self_name = nullptr) {
    LoweringState state(arena);
    state.primitives = primitives;
    state.cache_bridge = cache_bridge;
    state.cache_strings = cache_strings;
    if (self_name && !self_name->empty()) {
        state.self_name = *self_name;
    }
    state.module = {};

    IRFunction top_func;
    top_func.id = 0;
    top_func.name = "__top__";
    top_func.entry_block = 0;
    top_func.blocks.push_back({0, {}, {}});
    state.cur_func = &top_func;
    state.cur_block = 0;
    state.local_count = 0;

    auto result_slot = lower_flat_expr(state, flat, pool, flat.root, cache, cache_hits);


    state.emit(IROpcode::Return, result_slot);
    top_func.local_count = state.local_count;
    auto top_id = state.module.add_function(std::move(top_func));
    state.module.entry_function_id = top_id;
    return state.module;
}


IRModule lower_to_ir(FlatAST& flat, StringPool& pool, ASTArena& arena,
                      const Primitives* primitives) {
    return lower_to_ir_impl(flat, pool, arena, nullptr, nullptr, primitives);
}


IRModule lower_to_ir_with_cache(
    FlatAST& flat, StringPool& pool, ASTArena& arena,
    const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>* cache,
    std::vector<std::string>* cache_hits,
    const Primitives* primitives,
    const std::unordered_map<std::string, std::vector<aura::ir::ClosureBridgeData>>* cache_bridge,
    const std::unordered_map<std::string, std::vector<std::string>>* cache_strings,
    const std::string* self_name) {
    return lower_to_ir_impl(flat, pool, arena, cache, cache_hits, primitives, cache_bridge, cache_strings, self_name);
}


std::string unparse_node(const FlatAST& flat, const StringPool& pool,
                           NodeId id, int indent) {
    if (id == NULL_NODE || id >= flat.size()) return "()";
    auto v = flat.get(id);
    auto indent_str = [](int d) { return std::string(static_cast<std::size_t>(d * 2), ' '); };

    switch (v.tag) {
    case NodeTag::LiteralInt:
        return std::to_string(v.int_value);

    case NodeTag::LiteralString: {
        auto s = pool.resolve(v.sym_id);
        std::string out = "\"";
        for (char c : s) {
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else out += c;
        }
        return out + "\"";
    }

    case NodeTag::Variable:
        return std::string(pool.resolve(v.sym_id));

    case NodeTag::Call: {


        auto callee = v.child(0);
        std::string s = "(";
        s += unparse_node(flat, pool, callee, indent + 1);

        for (std::size_t i = 1; i < v.children.size(); ++i) {
            auto arg_str = unparse_node(flat, pool, v.child(i), indent + 1);

            bool long_arg = arg_str.size() > 40 ||
                (s.size() + arg_str.size() + 2 > 80 && s.size() > 20);
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
            if (i > 0) s += " ";
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
        return "(set! " + std::string(pool.resolve(v.sym_id)) + " "
             + unparse_node(flat, pool, v.child(0), indent + 1) + ")";
    }

    case NodeTag::Pair: {
        return "(" + unparse_node(flat, pool, v.child(0), indent + 1)
             + " . " + unparse_node(flat, pool, v.child(1), indent + 1)
             + ")";
    }

    case NodeTag::Quote: {
        return "(quote " + unparse_node(flat, pool, v.child(0), indent + 1) + ")";
    }

    case NodeTag::TypeAnnotation: {
        return "(the " + std::string(pool.resolve(v.sym_id)) + " "
             + unparse_node(flat, pool, v.child(0), indent + 1) + ")";
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

}
