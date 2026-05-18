module aura.compiler.ir_executor;
import std;
import aura.compiler.value;

namespace aura::compiler {

using namespace aura::ir;
using namespace aura::diag;
using namespace types;

EvalResult IRInterpreter::execute() {
    if (module_.functions.empty())
        return std::unexpected(Diagnostic{ErrorKind::IRNoReturn, "empty module"});
    return execute_function(module_.entry(), {});
}

EvalResult IRInterpreter::execute_function(const IRFunction& func,
                                            const std::vector<EvalValue>& args) {
    auto local_count = func.local_count + std::max(std::size_t(64), args.size());
    std::vector<EvalValue> locals(local_count, make_void());
    return run_function(func, locals, args);
}

EvalResult IRInterpreter::run_function(const IRFunction& func,
                                        std::vector<EvalValue>& locals,
                                        const std::vector<EvalValue>& args) {
    if (func.blocks.empty())
        return std::unexpected(Diagnostic{ErrorKind::IRNoReturn, "empty function"});

    // ── Coercion helpers shared across arithmetic opcodes ────────
    auto coerce_i = [&](const types::EvalValue& v) -> std::int64_t {
        if (is_int(v)) return as_int(v);
        if (is_float(v)) return static_cast<std::int64_t>(as_float(v));
        if (is_string(v)) {
            auto idx = as_string_idx(v);
            if (idx < string_heap_.size()) {
                try { return static_cast<std::int64_t>(std::stoll(string_heap_[idx])); }
                catch (...) { return 0; }
            }
        }
        if (is_bool(v)) return as_bool(v) ? 1 : 0;
        return 0;
    };
    auto coerce_f = [&](const types::EvalValue& v) -> double {
        if (is_float(v)) return as_float(v);
        return static_cast<double>(coerce_i(v));
    };

    std::uint32_t current = func.entry_block;

    while (current < func.blocks.size()) {
        auto& block = func.blocks[current];

        for (auto& instr : block.instructions) {
            auto& ops = instr.operands;

            // ── Operand validation via kOpcodeInfo ─────────────────
            if constexpr (true) {
                auto idx = static_cast<std::size_t>(instr.opcode);
                if (idx < std::size(kOpcodeInfo)) {
                    auto& info = kOpcodeInfo[idx];
                    if (info.has_result_slot && ops[0] >= locals.size()) {
                        return std::unexpected(Diagnostic{
                            ErrorKind::IRCorruption,
                            std::format("{}: result slot {} out of bounds", info.name, ops[0])});
                    }
                }
            }

            switch (instr.opcode) {
            case IROpcode::Nop:
                break;

            case IROpcode::ConstI64: {
                std::int64_t val = (static_cast<std::int64_t>(ops[2]) << 32) | ops[1];
                locals[ops[0]] = make_int(val);
                break;
            }
            case IROpcode::ConstF64: {
                std::uint64_t bits = static_cast<std::uint64_t>(ops[1]) | (static_cast<std::uint64_t>(ops[2]) << 32);
                double val;
                std::memcpy(&val, &bits, sizeof(val));
                locals[ops[0]] = make_float(val);
                break;
            }
            case IROpcode::ConstString: {
                // Store string in both heaps: primitives (for primitives) and local (for coercion)
                auto& prim_heap = const_cast<std::vector<std::string>&>(primitives_.string_heap());
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
                    if (is_int(val) && as_int(val) < 0) {
                        auto cell_slot = static_cast<std::size_t>(-as_int(val) - 1);
                        if (cell_slot < locals.size())
                            locals[ops[0]] = locals[cell_slot];
                        else
                            locals[ops[0]] = make_void();
                    } else {
                        locals[ops[0]] = val;
                    }
                } else
                    locals[ops[0]] = make_void();
                break;

            case IROpcode::Add: {
                auto& a = locals[ops[1]]; auto& b = locals[ops[2]];
                if (is_float(a) || is_float(b))
                    locals[ops[0]] = make_float(coerce_f(a) + coerce_f(b));
                else
                    locals[ops[0]] = make_int(coerce_i(a) + coerce_i(b));
                break;
            }
            case IROpcode::Sub: {
                auto& a = locals[ops[1]]; auto& b = locals[ops[2]];
                if (is_float(a) || is_float(b))
                    locals[ops[0]] = make_float(coerce_f(a) - coerce_f(b));
                else
                    locals[ops[0]] = make_int(coerce_i(a) - coerce_i(b));
                break;
            }
            case IROpcode::Mul: {
                auto& a = locals[ops[1]]; auto& b = locals[ops[2]];
                if (is_float(a) || is_float(b))
                    locals[ops[0]] = make_float(coerce_f(a) * coerce_f(b));
                else
                    locals[ops[0]] = make_int(coerce_i(a) * coerce_i(b));
                break;
            }
            case IROpcode::Div: {
                auto& a = locals[ops[1]]; auto& b = locals[ops[2]];
                if (is_float(a) || is_float(b)) {
                    auto y = coerce_f(b);
                    if (y == 0.0)
                        return std::unexpected(Diagnostic{ErrorKind::DivisionByZero, "division by zero"});
                    locals[ops[0]] = make_float(coerce_f(a) / y);
                } else {
                    auto y = coerce_i(b);
                    if (y == 0)
                        return std::unexpected(Diagnostic{ErrorKind::DivisionByZero, "division by zero"});
                    locals[ops[0]] = make_int(coerce_i(a) / y);
                }
                break;
            }

            case IROpcode::Eq: {
                auto& a = locals[ops[1]]; auto& b = locals[ops[2]];
                if (is_float(a) || is_float(b)) {
                    double x = is_float(a) ? as_float(a) : static_cast<double>(as_int(a));
                    double y = is_float(b) ? as_float(b) : static_cast<double>(as_int(b));
                    locals[ops[0]] = make_bool(x == y);
                } else
                    locals[ops[0]] = make_bool(as_int(a) == as_int(b));
                break;
            }
            case IROpcode::Lt: {
                auto& a = locals[ops[1]]; auto& b = locals[ops[2]];
                if (is_float(a) || is_float(b)) {
                    double x = is_float(a) ? as_float(a) : static_cast<double>(as_int(a));
                    double y = is_float(b) ? as_float(b) : static_cast<double>(as_int(b));
                    locals[ops[0]] = make_bool(x < y);
                } else
                    locals[ops[0]] = make_bool(as_int(a) < as_int(b));
                break;
            }
            case IROpcode::Gt: {
                auto& a = locals[ops[1]]; auto& b = locals[ops[2]];
                if (is_float(a) || is_float(b)) {
                    double x = is_float(a) ? as_float(a) : static_cast<double>(as_int(a));
                    double y = is_float(b) ? as_float(b) : static_cast<double>(as_int(b));
                    locals[ops[0]] = make_bool(x > y);
                } else
                    locals[ops[0]] = make_bool(as_int(a) > as_int(b));
                break;
            }
            case IROpcode::Le: {
                auto& a = locals[ops[1]]; auto& b = locals[ops[2]];
                if (is_float(a) || is_float(b)) {
                    double x = is_float(a) ? as_float(a) : static_cast<double>(as_int(a));
                    double y = is_float(b) ? as_float(b) : static_cast<double>(as_int(b));
                    locals[ops[0]] = make_bool(x <= y);
                } else
                    locals[ops[0]] = make_bool(as_int(a) <= as_int(b));
                break;
            }
            case IROpcode::Ge: {
                auto& a = locals[ops[1]]; auto& b = locals[ops[2]];
                if (is_float(a) || is_float(b)) {
                    double x = is_float(a) ? as_float(a) : static_cast<double>(as_int(a));
                    double y = is_float(b) ? as_float(b) : static_cast<double>(as_int(b));
                    locals[ops[0]] = make_bool(x >= y);
                } else
                    locals[ops[0]] = make_bool(as_int(a) >= as_int(b));
                break;
            }

            case IROpcode::And:
                locals[ops[0]] = make_bool(is_truthy(locals[ops[1]]) && is_truthy(locals[ops[2]]));
                break;
            case IROpcode::Or:
                locals[ops[0]] = make_bool(is_truthy(locals[ops[1]]) || is_truthy(locals[ops[2]]));
                break;
            case IROpcode::Not:
                locals[ops[0]] = make_bool(!is_truthy(locals[ops[1]]));
                break;

            case IROpcode::CastOp: {
                // CastOp: result_slot=ops[0], value_slot=ops[1], type_tag=ops[2]
                // type_tag: 0=Int, 1=String, 2=Bool, 3+=Dynamic
                auto& val = locals[ops[1]];
                
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
                        } else {
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
                        } else {
                            locals[ops[0]] = val;
                        }
                        break;
                    }
                    case 2: { // Coerce to Bool
                        locals[ops[0]] = make_bool(is_truthy(val));
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

            case IROpcode::Jump:
                current = ops[0];
                goto next_block;

            case IROpcode::MakePair: {
                // Use the evaluator's cons primitive so pairs are in the same table
                auto pfn = primitives_.lookup("cons");
                if (pfn)
                    locals[ops[0]] = (*pfn)({locals[ops[1]], locals[ops[2]]});
                else
                    locals[ops[0]] = make_void();
                break;
            }
            case IROpcode::Car: {
                auto pfn = primitives_.lookup("car");
                if (pfn)
                    locals[ops[0]] = (*pfn)({locals[ops[1]]});
                else
                    locals[ops[0]] = make_void();
                break;
            }
            case IROpcode::Cdr: {
                auto pfn = primitives_.lookup("cdr");
                if (pfn)
                    locals[ops[0]] = (*pfn)({locals[ops[1]]});
                else
                    locals[ops[0]] = make_void();
                break;
            }

            case IROpcode::PrimCall: {
                auto prim_id = static_cast<PrimId>(ops[0]);
                auto arg_base = unpack_hi(ops[1]);
                auto arg_count = unpack_lo(ops[1]);
                std::vector<EvalValue> pargs;
                for (std::uint32_t pi = 0; pi < arg_count; ++pi)
                    pargs.push_back(locals[arg_base + pi]);
                EvalResult presult = make_int(0);
                // Map PrimId to registered primitive name
                static const char* prim_names[] = {
                    "string-append", "string-length", "string-ref",
                    "substring", "string=?", "string<?",
                    "number->string", "string->number",
                    "display", "write", "newline",
                    "error", "assert",
                    "read", "read-file", "write-file", "file-exists?",
                    "gensym",
                    "apply",
                    "vector", "vector-ref", "vector-set!",
                    "vector-length", "vector?", "make-vector",
                    "import",
                };
                auto idx = static_cast<std::size_t>(prim_id);
                if (idx < std::size(prim_names)) {
                    auto pfn = primitives_.lookup(prim_names[idx]);
                    if (pfn) presult = (*pfn)(pargs);
                }
                if (presult) locals[ops[2]] = *presult;
                break;
            }

            case IROpcode::Call: {
                auto& callee_val = locals[ops[0]];
                auto arg_base = ops[1];
                auto arg_count = ops[2];

                std::vector<EvalValue> call_args;
                for (std::uint32_t i = 0; i < arg_count; ++i)
                    call_args.push_back(locals[arg_base + i]);

                if (is_closure(callee_val)) {
                    auto closure_id = as_closure_id(callee_val);
                    auto it = runtime_closures_.find(closure_id);
                    if (it == runtime_closures_.end())
                        return std::unexpected(Diagnostic{ErrorKind::InvalidClosure, "invalid closure reference"});

                    auto& closure = it->second;
                    if (closure.func_id >= module_.functions.size())
                        return std::unexpected(Diagnostic{ErrorKind::IRCorruption, "invalid closure function id"});

                    auto& callee_func = module_.functions[closure.func_id];

                    std::vector<EvalValue> all_args;
                    for (auto& ev : closure.env) all_args.push_back(ev);
                    for (auto& a : call_args) all_args.push_back(a);

                    auto result = execute_function(callee_func, all_args);
                    if (!result) return result;
                    locals[ops[3]] = *result;
                } else if (is_primitive(callee_val)) {
                    // Primitive function call — look up and invoke
                    auto slot = as_primitive_slot(callee_val);
                    auto prim_name = primitives_.name_for_slot(slot);
                    auto pfn = primitives_.lookup(prim_name);
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
                IRClosure ircl{ops[1], std::vector<EvalValue>(ops[2], make_void())};
                // Copy bridge data from IRModule if available
                if (ops[1] < module_.closure_bridge.size()) {
                    auto& bd = module_.closure_bridge[ops[1]];
                    ircl.flat = bd.flat;
                    ircl.pool = bd.pool;
                    ircl.body_id = bd.body_id;
                    if (ops[1] < module_.functions.size())
                        ircl.params = module_.functions[ops[1]].params;
                }
                runtime_closures_[id] = std::move(ircl);
                locals[ops[0]] = make_closure(id);
                break;
            }

            case IROpcode::Capture: {
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
                auto& closure_val = locals[ops[0]];
                if (is_closure(closure_val)) {
                    auto closure_id = as_closure_id(closure_val);
                    auto it = runtime_closures_.find(closure_id);
                    if (it != runtime_closures_.end() && ops[1] < it->second.env.size())
                        // Store negative offset to indicate cell reference
                        it->second.env[ops[1]] = make_int(-1 - static_cast<std::int64_t>(ops[2]));
                }
                break;
            }

            case IROpcode::Apply: {
                auto& closure_val = locals[ops[0]];
                auto arg_count = ops[1];
                std::vector<EvalValue> apply_args;
                for (std::uint32_t i = 0; i < arg_count; ++i)
                    apply_args.push_back(locals[ops[0] + i + 1]);

                if (is_closure(closure_val)) {
                    auto closure_id = as_closure_id(closure_val);
                    auto it = runtime_closures_.find(closure_id);
                    if (it == runtime_closures_.end())
                        return std::unexpected(Diagnostic{ErrorKind::InvalidClosure, "invalid closure in apply"});

                    auto& closure = it->second;
                    if (closure.func_id >= module_.functions.size())
                        return std::unexpected(Diagnostic{ErrorKind::IRCorruption, "invalid function id in apply"});

                    auto& callee_func = module_.functions[closure.func_id];
                    std::vector<EvalValue> all_args;
                    for (auto& ev : closure.env) all_args.push_back(ev);
                    for (auto& a : apply_args) all_args.push_back(a);

                    auto result = execute_function(callee_func, all_args);
                    if (!result) return result;
                    locals[ops[2]] = *result;
                } else {
                    locals[ops[2]] = closure_val;
                }
                break;
            }

            case IROpcode::NewCell: {
                auto cell_id = next_cell_id_++;
                cell_heap_[cell_id] = make_void();
                locals[ops[0]] = make_cell(cell_id);
                break;
            }

            case IROpcode::CellSet: {
                auto& cell_id_val = locals[ops[0]];
                if (is_cell(cell_id_val)) {
                    auto cell_id = as_cell_id(cell_id_val);
                    cell_heap_[cell_id] = locals[ops[1]];
                }
                break;
            }

            case IROpcode::CellGet: {
                auto& cell_id_val = locals[ops[1]];
                if (is_cell(cell_id_val)) {
                    auto cell_id = as_cell_id(cell_id_val);
                    auto it = cell_heap_.find(cell_id);
                    locals[ops[0]] = (it != cell_heap_.end()) ? it->second : make_void();
                } else {
                    locals[ops[0]] = make_void();
                }
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
                // Load a primitive value by slot index from the Primitives table
                auto prim_slot = ops[1];
                auto prim_name = primitives_.name_for_slot(prim_slot);
                auto pfn = primitives_.lookup(prim_name);
                if (pfn) {
                    locals[ops[0]] = types::make_primitive(prim_slot);
                } else {
                    locals[ops[0]] = types::make_void();
                }
                break;
            }

            default:
                break;
            }
        }
        ++current;
        next_block:;
    }

    return std::unexpected(Diagnostic{ErrorKind::IRNoReturn, "no return"});
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
ClosureSnapshot IRInterpreter::make_snapshot(std::uint64_t id,
                                               const IRClosure& closure) const {
    std::string fname;
    std::vector<std::string> params, free_vars;
    if (closure.func_id < module_.functions.size()) {
        auto& f = module_.functions[closure.func_id];
        fname = f.name;
        params = f.params;
        free_vars = f.free_vars;
    } else {
        fname = "<unknown>";
    }
    return ClosureSnapshot{
        .id            = id,
        .func_id       = closure.func_id,
        .func_name     = std::move(fname),
        .func_params   = std::move(params),
        .func_free_vars = std::move(free_vars),
        .env           = closure.env
    };
}

std::vector<CellSnapshot> IRInterpreter::list_cells() const {
    std::vector<CellSnapshot> result;
    result.reserve(cell_heap_.size());
    for (auto& [id, val] : cell_heap_) {
        result.push_back(CellSnapshot{.id = id, .value = val});
    }
    return result;
}

} // namespace aura::compiler
