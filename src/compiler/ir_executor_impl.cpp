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

    std::uint32_t current = func.entry_block;

    while (current < func.blocks.size()) {
        auto& block = func.blocks[current];

        for (auto& instr : block.instructions) {
            auto& ops = instr.operands;

            switch (instr.opcode) {
            case IROpcode::Nop:
                break;

            case IROpcode::ConstI64: {
                std::int64_t val = (static_cast<std::int64_t>(ops[2]) << 32) | ops[1];
                locals[ops[0]] = make_int(val);
                break;
            }
            case IROpcode::ConstString: {
                // Store string in the shared string heap so primitives can find it
                auto& heap = const_cast<std::vector<std::string>&>(primitives_.string_heap());
                auto sidx = heap.size();
                if (ops[1] < module_.string_pool.size())
                    heap.push_back(module_.string_pool[ops[1]]);
                else
                    heap.push_back("");
                locals[ops[0]] = make_string(sidx);
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

            case IROpcode::Add:
                locals[ops[0]] = make_int(as_int(locals[ops[1]]) + as_int(locals[ops[2]]));
                break;
            case IROpcode::Sub:
                locals[ops[0]] = make_int(as_int(locals[ops[1]]) - as_int(locals[ops[2]]));
                break;
            case IROpcode::Mul:
                locals[ops[0]] = make_int(as_int(locals[ops[1]]) * as_int(locals[ops[2]]));
                break;
            case IROpcode::Div:
                if (as_int(locals[ops[2]]) == 0)
                    return std::unexpected(Diagnostic{ErrorKind::DivisionByZero, "division by zero"});
                locals[ops[0]] = make_int(as_int(locals[ops[1]]) / as_int(locals[ops[2]]));
                break;

            case IROpcode::Eq:
                locals[ops[0]] = make_int(as_int(locals[ops[1]]) == as_int(locals[ops[2]]) ? 1 : 0);
                break;
            case IROpcode::Lt:
                locals[ops[0]] = make_int(as_int(locals[ops[1]]) < as_int(locals[ops[2]]) ? 1 : 0);
                break;
            case IROpcode::Gt:
                locals[ops[0]] = make_int(as_int(locals[ops[1]]) > as_int(locals[ops[2]]) ? 1 : 0);
                break;
            case IROpcode::Le:
                locals[ops[0]] = make_int(as_int(locals[ops[1]]) <= as_int(locals[ops[2]]) ? 1 : 0);
                break;
            case IROpcode::Ge:
                locals[ops[0]] = make_int(as_int(locals[ops[1]]) >= as_int(locals[ops[2]]) ? 1 : 0);
                break;

            case IROpcode::And:
                locals[ops[0]] = make_int(is_truthy(locals[ops[1]]) && is_truthy(locals[ops[2]]) ? 1 : 0);
                break;
            case IROpcode::Or:
                locals[ops[0]] = make_int(is_truthy(locals[ops[1]]) || is_truthy(locals[ops[2]]) ? 1 : 0);
                break;
            case IROpcode::Not:
                locals[ops[0]] = make_int(!is_truthy(locals[ops[1]]) ? 1 : 0);
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
                } else {
                    locals[ops[3]] = callee_val;
                }
                break;
            }

            case IROpcode::Return:
                return locals[ops[0]];

            case IROpcode::MakeClosure: {
                auto id = next_closure_id_++;
                runtime_closures_[id] = IRClosure{ops[1], std::vector<EvalValue>(ops[2], make_void())};
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
