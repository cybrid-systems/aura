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

    const auto& entry = module_.entry();
    auto local_count = entry.local_count + 64;
    call_stack_.clear();
    call_stack_.push_back({.func = &entry,
                           .current_block = entry.entry_block,
                           .locals = std::vector<EvalValue>(local_count, make_void()),
                           .args = {},
                           .resume_instr = 0,
                           .is_top_level = true,
                           .result_slot = 0});

    while (!call_stack_.empty()) {
        auto& frame = call_stack_.back();
        auto result = run_function(*frame.func, frame.locals, frame.args);

        if (std::holds_alternative<PendingCall>(result)) {
            auto& pc = std::get<PendingCall>(result);
            auto new_count = pc.func->local_count + std::max(std::size_t(64), pc.args.size());
            std::vector<EvalValue> new_locals(new_count, make_void());
            call_stack_.push_back({.func = pc.func,
                                   .current_block = pc.func->entry_block,
                                   .locals = std::move(new_locals),
                                   .args = std::move(pc.args),
                                   .resume_instr = 0,
                                   .is_top_level = false,
                                   .result_slot = pc.result_slot});
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
                                           const std::vector<EvalValue>& args) {
    auto local_count = func.local_count + std::max(std::size_t(64), args.size());
    std::vector<EvalValue> locals(local_count, make_void());
    auto result = run_function(func, locals, args);
    if (std::holds_alternative<EvalResult>(result))
        return std::get<EvalResult>(std::move(result));
    return std::unexpected(Diagnostic{ErrorKind::IRCorruption, "unexpected pending call"});
}

IRInterpreter::RunResult IRInterpreter::run_function(const IRFunction& func,
                                                     std::vector<EvalValue>& locals,
                                                     const std::vector<EvalValue>& args) {
    if (func.blocks.empty())
        return std::unexpected(Diagnostic{ErrorKind::IRNoReturn, "empty function"}.with_suggestion(
            "the function body is empty"));

    // ── Coercion helpers shared across arithmetic opcodes ────────
    // Report CastOp blame with source location
    auto report_blame = [&](const char* expected_kind, const char* got_kind,
                            std::uint32_t blame_loc) {
        std::string msg =
            std::string("runtime type mismatch: expected ") + expected_kind + ", got " + got_kind;
        auto diag = Diagnostic(ErrorKind::TypeError, std::move(msg))
                        .with_blame(BlameInfo{BlameParty::Implicit, "", "runtime"});
        if (blame_loc != 0) {
            auto line = (blame_loc >> 16) & 0xFFFFu;
            auto col = blame_loc & 0xFFFFu;
            diag.location = SourceLocation{line, col, 0};
        }
        std::cerr << diag.format() << std::endl;
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
                    std::println(std::cerr,
                                 "error: type mismatch — expected Int, got String '{}'",
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
                    std::uint64_t bits = static_cast<std::uint64_t>(ops[1]) |
                                         (static_cast<std::uint64_t>(ops[2]) << 32);
                    double val;
                    std::memcpy(&val, &bits, sizeof(val));
                    locals[ops[0]] = make_float(val);
                    break;
                }
                case IROpcode::ConstString: {
                    // Store string in both heaps: primitives (for primitives) and local (for
                    // coercion)
                    auto& prim_heap =
                        const_cast<std::vector<std::string>&>(primitives_.string_heap());
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

                case IROpcode::Add: {
                    auto& a = locals[ops[1]];
                    auto& b = locals[ops[2]];
                    if (is_float(a) || is_float(b))
                        locals[ops[0]] = make_float(coerce_f(a) + coerce_f(b));
                    else
                        locals[ops[0]] = make_int(coerce_i(a) + coerce_i(b));
                    break;
                }
                case IROpcode::Sub: {
                    auto& a = locals[ops[1]];
                    auto& b = locals[ops[2]];
                    if (is_float(a) || is_float(b))
                        locals[ops[0]] = make_float(coerce_f(a) - coerce_f(b));
                    else
                        locals[ops[0]] = make_int(coerce_i(a) - coerce_i(b));
                    break;
                }
                case IROpcode::Mul: {
                    auto& a = locals[ops[1]];
                    auto& b = locals[ops[2]];
                    if (is_float(a) || is_float(b))
                        locals[ops[0]] = make_float(coerce_f(a) * coerce_f(b));
                    else
                        locals[ops[0]] = make_int(coerce_i(a) * coerce_i(b));
                    break;
                }
                case IROpcode::Div: {
                    auto& a = locals[ops[1]];
                    auto& b = locals[ops[2]];
                    if (is_float(a) || is_float(b)) {
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

                case IROpcode::Eq: {
                    auto& a = locals[ops[1]];
                    auto& b = locals[ops[2]];
                    if (is_float(a) || is_float(b)) {
                        double x = is_float(a) ? as_float(a) : static_cast<double>(as_int(a));
                        double y = is_float(b) ? as_float(b) : static_cast<double>(as_int(b));
                        locals[ops[0]] = make_bool(x == y);
                    } else
                        locals[ops[0]] = make_bool(as_int(a) == as_int(b));
                    break;
                }
                case IROpcode::Lt: {
                    auto& a = locals[ops[1]];
                    auto& b = locals[ops[2]];
                    if (is_float(a) || is_float(b)) {
                        double x = is_float(a) ? as_float(a) : static_cast<double>(as_int(a));
                        double y = is_float(b) ? as_float(b) : static_cast<double>(as_int(b));
                        locals[ops[0]] = make_bool(x < y);
                    } else
                        locals[ops[0]] = make_bool(as_int(a) < as_int(b));
                    break;
                }
                case IROpcode::Gt: {
                    auto& a = locals[ops[1]];
                    auto& b = locals[ops[2]];
                    if (is_float(a) || is_float(b)) {
                        double x = is_float(a) ? as_float(a) : static_cast<double>(as_int(a));
                        double y = is_float(b) ? as_float(b) : static_cast<double>(as_int(b));
                        locals[ops[0]] = make_bool(x > y);
                    } else
                        locals[ops[0]] = make_bool(as_int(a) > as_int(b));
                    break;
                }
                case IROpcode::Le: {
                    auto& a = locals[ops[1]];
                    auto& b = locals[ops[2]];
                    if (is_float(a) || is_float(b)) {
                        double x = is_float(a) ? as_float(a) : static_cast<double>(as_int(a));
                        double y = is_float(b) ? as_float(b) : static_cast<double>(as_int(b));
                        locals[ops[0]] = make_bool(x <= y);
                    } else
                        locals[ops[0]] = make_bool(as_int(a) <= as_int(b));
                    break;
                }
                case IROpcode::Ge: {
                    auto& a = locals[ops[1]];
                    auto& b = locals[ops[2]];
                    if (is_float(a) || is_float(b)) {
                        double x = is_float(a) ? as_float(a) : static_cast<double>(as_int(a));
                        double y = is_float(b) ? as_float(b) : static_cast<double>(as_int(b));
                        locals[ops[0]] = make_bool(x >= y);
                    } else
                        locals[ops[0]] = make_bool(as_int(a) >= as_int(b));
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
                    // type_tag: 0=Int, 1=String, 2=Bool, 3+=Dynamic
                    auto& val = locals[ops[1]];
                    auto blame_loc = ops[3];

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
                                report_blame("Int", "unknown", blame_loc);
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
                                report_blame("Float", "unknown", 0);
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

                case IROpcode::Raise: {
                    // Raise: result_slot=ops[0], cause_slot=ops[1]
                    // Create an error value by calling the raise primitive
                    auto pfn = primitives_.lookup("raise");
                    if (pfn) {
                        locals[ops[0]] = (*pfn)({locals[ops[1]]});
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
                    auto prim_id = static_cast<PrimId>(ops[0]);
                    auto arg_base = unpack_hi(ops[1]);
                    auto arg_count = unpack_lo(ops[1]);
                    std::vector<EvalValue> pargs;
                    for (std::uint32_t pi = 0; pi < arg_count; ++pi)
                        pargs.push_back(locals[arg_base + pi]);
                    EvalResult presult = make_int(0);
                    // Map PrimId to registered primitive name via kPrimNames
                    auto idx = static_cast<std::size_t>(prim_id);
                    if (idx < std::size(kPrimNames)) {
                        auto pfn = primitives_.lookup(std::string(kPrimNames[idx]));
                        if (pfn)
                            presult = (*pfn)(pargs);
                    }
                    if (presult)
                        locals[ops[2]] = *presult;
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
                            return std::unexpected(
                                Diagnostic{ErrorKind::InvalidClosure, "invalid closure reference"}
                                    .with_suggestion("the function was redefined (--hot-swap)"));

                        auto& closure = it->second;
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
                            it->second.env[ops[1]] =
                                make_int(-1 - static_cast<std::int64_t>(ops[2]));
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
                            return std::unexpected(
                                Diagnostic{ErrorKind::InvalidClosure, "invalid closure in apply"}
                                    .with_suggestion("the function was redefined (--hot-swap)"));

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

                // ── M4 Linear ownership opcodes ────────────────────────
                case IROpcode::LinearWrap: {
                    // Wrap value in a linear container with refcount=1
                    auto inner = locals[ops[1]];
                    auto lin_id = next_linear_id_++;
                    linear_heap_[lin_id] = {inner, 1};
                    locals[ops[0]] = types::make_linear(lin_id);
                    break;
                }
                case IROpcode::MoveOp: {
                    // Move ownership: decrement source refcount, pass value through
                    auto val = locals[ops[1]];
                    if (types::is_linear(val)) {
                        auto lin_id = types::as_linear_id(val);
                        auto it = linear_heap_.find(lin_id);
                        if (it != linear_heap_.end()) {
                            auto result = it->second.value;
                            if (--it->second.ref_count == 0)
                                linear_heap_.erase(it);
                            locals[ops[0]] = result;
                        } else {
                            locals[ops[0]] = locals[ops[1]];
                        }
                    } else {
                        locals[ops[0]] = val;
                    }
                    break;
                }
                case IROpcode::BorrowOp: {
                    // Immutable borrow: increment refcount
                    auto val = locals[ops[1]];
                    if (types::is_linear(val)) {
                        auto lin_id = types::as_linear_id(val);
                        auto it = linear_heap_.find(lin_id);
                        if (it != linear_heap_.end()) {
                            it->second.ref_count++;
                            locals[ops[0]] = it->second.value;
                        } else {
                            locals[ops[0]] = val;
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
                        auto it = linear_heap_.find(lin_id);
                        if (it != linear_heap_.end()) {
                            locals[ops[0]] = it->second.value;
                        } else {
                            locals[ops[0]] = val;
                        }
                    } else {
                        locals[ops[0]] = val;
                    }
                    break;
                }
                case IROpcode::DropOp: {
                    // Explicit destruct: decrement refcount, erase if zero
                    auto val = locals[ops[0]];
                    if (types::is_linear(val)) {
                        auto lin_id = types::as_linear_id(val);
                        auto it = linear_heap_.find(lin_id);
                        if (it != linear_heap_.end()) {
                            if (--it->second.ref_count == 0)
                                linear_heap_.erase(it);
                        }
                    }
                    break;
                }
                case IROpcode::RefCountOp: {
                    // Runtime refcount operation: ops[2] = 1 for inc, 0 for dec
                    auto val = locals[ops[1]];
                    if (types::is_linear(val)) {
                        auto lin_id = types::as_linear_id(val);
                        auto it = linear_heap_.find(lin_id);
                        if (it != linear_heap_.end()) {
                            if (ops[2] == 1)
                                it->second.ref_count++;
                            else if (it->second.ref_count > 0)
                                it->second.ref_count--;
                        }
                    }
                    locals[ops[0]] = val;
                    break;
                }

                default:
                    break;
            }

            // ── Runtime type assertion (strict mode only) ─────────
            if (strict_mode_ && instr.type_id != 0) {
                auto idx = static_cast<std::size_t>(instr.opcode);
                bool has_result = idx < std::size(kOpcodeInfo) && kOpcodeInfo[idx].has_result_slot;
                if (has_result) {
                    auto rv = check_runtime_type(instr.type_id, locals[ops[0]],
                                                 std::string(kOpcodeInfo[idx].name));
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
    if (!strict_mode_ || !type_registry_ || type_id == 0)
        return std::nullopt;

    auto runtime_tag = value_type_tag(val);
    if (!runtime_tag)
        return std::nullopt; // unknown runtime type, skip

    auto expected_tid = aura::core::TypeId{type_id, 1};
    if (!expected_tid.valid())
        return std::nullopt;
    auto expected_tag = type_registry_->tag_of(expected_tid);

    if (*runtime_tag == expected_tag)
        return std::nullopt; // match

    return aura::diag::Diagnostic{
        aura::diag::ErrorKind::TypeError,
        std::format("runtime type mismatch in {}: expected {} got value of different type", context,
                    type_registry_->name_of(expected_tid))};
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
                    if (type_registry_) {
                        return_type = std::string(
                            type_registry_->name_of(aura::core::TypeId{instr.type_id, 1}));
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
    for (auto& [id, val] : cell_heap_) {
        result.push_back(CellSnapshot{.id = id, .value = val});
    }
    return result;
}

} // namespace aura::compiler
