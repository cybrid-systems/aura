module aura.compiler.ir_interpreter;
import std;

namespace aura::compiler {

using namespace aura::ir;
using namespace aura::diag;

EvalResult IRInterpreter::execute() {
    if (module_.functions.empty())
        return std::unexpected(Diagnostic{ErrorKind::IRNoReturn, "empty module"});
    return execute_function(module_.entry(), {});
}

EvalResult IRInterpreter::execute_function(const IRFunction& func,
                                            const std::vector<std::int64_t>& args) {
    auto local_count = func.local_count + std::max(std::size_t(64), args.size());
    std::vector<std::int64_t> locals(local_count, 0);
    return run_function(func, locals, args);
}

EvalResult IRInterpreter::run_function(const IRFunction& func,
                                        std::vector<std::int64_t>& locals,
                                        const std::vector<std::int64_t>& args) {
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
                locals[ops[0]] = val;
                break;
            }

            case IROpcode::Local:
                locals[ops[0]] = locals[ops[1]];
                break;

            case IROpcode::Arg:
                if (ops[1] < args.size()) {
                    auto val = args[ops[1]];
                    if (val < 0) {
                        auto cell_slot = static_cast<std::size_t>(-val - 1);
                        if (cell_slot < locals.size())
                            locals[ops[0]] = locals[cell_slot];
                        else
                            locals[ops[0]] = 0;
                    } else {
                        locals[ops[0]] = val;
                    }
                } else
                    locals[ops[0]] = 0;
                break;

            case IROpcode::Add:
                locals[ops[0]] = locals[ops[1]] + locals[ops[2]];
                break;
            case IROpcode::Sub:
                locals[ops[0]] = locals[ops[1]] - locals[ops[2]];
                break;
            case IROpcode::Mul:
                locals[ops[0]] = locals[ops[1]] * locals[ops[2]];
                break;
            case IROpcode::Div:
                if (locals[ops[2]] == 0)
                    return std::unexpected(Diagnostic{ErrorKind::DivisionByZero, "division by zero"});
                locals[ops[0]] = locals[ops[1]] / locals[ops[2]];
                break;

            case IROpcode::Eq:
                locals[ops[0]] = (locals[ops[1]] == locals[ops[2]]) ? 1 : 0;
                break;
            case IROpcode::Lt:
                locals[ops[0]] = (locals[ops[1]] < locals[ops[2]]) ? 1 : 0;
                break;
            case IROpcode::Gt:
                locals[ops[0]] = (locals[ops[1]] > locals[ops[2]]) ? 1 : 0;
                break;
            case IROpcode::Le:
                locals[ops[0]] = (locals[ops[1]] <= locals[ops[2]]) ? 1 : 0;
                break;
            case IROpcode::Ge:
                locals[ops[0]] = (locals[ops[1]] >= locals[ops[2]]) ? 1 : 0;
                break;

            case IROpcode::And:
                locals[ops[0]] = (locals[ops[1]] && locals[ops[2]]) ? 1 : 0;
                break;
            case IROpcode::Or:
                locals[ops[0]] = (locals[ops[1]] || locals[ops[2]]) ? 1 : 0;
                break;
            case IROpcode::Not:
                locals[ops[0]] = (!locals[ops[1]]) ? 1 : 0;
                break;

            case IROpcode::Branch:
                current = (locals[ops[0]] != 0) ? ops[1] : ops[2];
                goto next_block;

            case IROpcode::Jump:
                current = ops[0];
                goto next_block;

            case IROpcode::Call: {
                auto callee_val = locals[ops[0]];
                auto arg_base = ops[1];
                auto arg_count = ops[2];

                std::vector<std::int64_t> call_args;
                for (std::uint32_t i = 0; i < arg_count; ++i)
                    call_args.push_back(locals[arg_base + i]);

                constexpr std::int64_t CLOSURE_SENTINEL = 0x1000000;
                if (static_cast<std::uint64_t>(callee_val) >= static_cast<std::uint64_t>(CLOSURE_SENTINEL)) {
                    auto closure_id = static_cast<std::uint64_t>(callee_val - CLOSURE_SENTINEL);
                    auto it = runtime_closures_.find(closure_id);
                    if (it == runtime_closures_.end())
                        return std::unexpected(Diagnostic{ErrorKind::InvalidClosure, "invalid closure reference"});

                    auto& closure = it->second;
                    if (closure.func_id >= module_.functions.size())
                        return std::unexpected(Diagnostic{ErrorKind::IRCorruption, "invalid closure function id"});

                    auto& callee_func = module_.functions[closure.func_id];

                    std::vector<std::int64_t> all_args;
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
                runtime_closures_[id] = IRClosure{ops[1], std::vector<std::int64_t>(ops[2], 0)};
                constexpr std::int64_t CLOSURE_SENTINEL = 0x1000000;
                locals[ops[0]] = CLOSURE_SENTINEL + static_cast<std::int64_t>(id);
                break;
            }

            case IROpcode::Capture: {
                auto closure_val = locals[ops[0]];
                constexpr std::int64_t CLOSURE_SENTINEL = 0x1000000;
                if (static_cast<std::uint64_t>(closure_val) >= static_cast<std::uint64_t>(CLOSURE_SENTINEL)) {
                    auto closure_id = static_cast<std::uint64_t>(closure_val - CLOSURE_SENTINEL);
                    auto it = runtime_closures_.find(closure_id);
                    if (it != runtime_closures_.end() && ops[1] < it->second.env.size())
                        it->second.env[ops[1]] = locals[ops[2]];
                }
                break;
            }

            case IROpcode::CaptureRef: {
                auto closure_val = locals[ops[0]];
                constexpr std::int64_t CLOSURE_SENTINEL = 0x1000000;
                if (static_cast<std::uint64_t>(closure_val) >= static_cast<std::uint64_t>(CLOSURE_SENTINEL)) {
                    auto closure_id = static_cast<std::uint64_t>(closure_val - CLOSURE_SENTINEL);
                    auto it = runtime_closures_.find(closure_id);
                    if (it != runtime_closures_.end() && ops[1] < it->second.env.size())
                        it->second.env[ops[1]] = -1 - static_cast<std::int64_t>(ops[2]);
                }
                break;
            }

            case IROpcode::Apply: {
                auto closure_val = locals[ops[0]];
                auto arg_count = ops[1];
                std::vector<std::int64_t> apply_args;
                for (std::uint32_t i = 0; i < arg_count; ++i)
                    apply_args.push_back(locals[ops[0] + i + 1]);

                constexpr std::int64_t CLOSURE_SENTINEL = 0x1000000;
                if (static_cast<std::uint64_t>(closure_val) >= static_cast<std::uint64_t>(CLOSURE_SENTINEL)) {
                    auto closure_id = static_cast<std::uint64_t>(closure_val - CLOSURE_SENTINEL);
                    auto it = runtime_closures_.find(closure_id);
                    if (it == runtime_closures_.end())
                        return std::unexpected(Diagnostic{ErrorKind::InvalidClosure, "invalid closure in apply"});

                    auto& closure = it->second;
                    if (closure.func_id >= module_.functions.size())
                        return std::unexpected(Diagnostic{ErrorKind::IRCorruption, "invalid function id in apply"});

                    auto& callee_func = module_.functions[closure.func_id];
                    std::vector<std::int64_t> all_args;
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
                cell_heap_[cell_id] = 0;
                locals[ops[0]] = static_cast<std::int64_t>(cell_id);  // result_slot = cell_id
                break;
            }

            case IROpcode::CellSet: {
                // ops[0] = cell_id_slot, ops[1] = value_slot
                auto cell_id = static_cast<std::uint64_t>(locals[ops[0]]);
                cell_heap_[cell_id] = locals[ops[1]];
                break;
            }

            case IROpcode::CellGet: {
                // ops[0] = result_slot, ops[1] = cell_id_slot
                auto cell_id = static_cast<std::uint64_t>(locals[ops[1]]);
                locals[ops[0]] = cell_heap_[cell_id];
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
