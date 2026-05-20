# 0 "/home/dev/code/aura/src/compiler/pass_manager.ixx"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/compiler/pass_manager.ixx"
export module aura.compiler.pass_manager;
import std;
import aura.core;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.compute_kind;
import aura.compiler.arity;
import aura.compiler.type_checker;
import aura.diag;

namespace aura::compiler {


export template <typename P>
concept Pass = requires(P& p, aura::ir::IRModule& m) {
    { p.run(m) } -> std::same_as<void>;
    { p.has_error() } -> std::convertible_to<bool>;
};


export template <Pass... Passes>
bool run_pipeline(aura::ir::IRModule& mod, Passes&... passes) {
    return (run_one(mod, passes) && ...);
}


export template <Pass P>
bool run_one(aura::ir::IRModule& mod, P& pass) {
    pass.run(mod);
    return !pass.has_error();
}


export class ComputeKindWrap {
public:
    void run(aura::ir::IRModule& module) {
        results_.clear();
        for (auto& func : module.functions)
            results_.push_back(aura::compiler::compute_kind(func));
    }


    ComputeKindResult compute_function(const aura::ir::IRFunction& func) {
        return aura::compiler::compute_kind(func);
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "compute-kind"; }
    const std::vector<ComputeKindResult>& results() const { return results_; }

private:
    std::vector<ComputeKindResult> results_;
};


export class ArityWrap {
public:
    void run(aura::ir::IRModule& module) {
        result_ = aura::compiler::check_arity(module);
    }

    bool has_error() const { return result_.has_error; }
    std::string_view name() const { return "arity"; }
    const ArityCheckResult& result() const { return result_; }

private:
    ArityCheckResult result_;
};


export class ConstantFoldingWrap {
public:
    void run(aura::ir::IRModule& module) {
        folded_ = 0;
        for (auto& func : module.functions) {
            for (auto& block : func.blocks) {
                fold_block(block);
            }
        }
    }




    std::size_t fold_function(aura::ir::IRFunction& func) {
        std::size_t before = folded_;
        for (auto& block : func.blocks) {
            known_.clear();
            fold_block(block);
        }
        return folded_ - before;
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "const-fold"; }
    std::size_t folded_count() const { return folded_; }

private:
    void fold_block(aura::ir::BasicBlock& block) {
        known_.clear();
        for (auto& instr : block.instructions) {
            auto& ops = instr.operands;
            switch (instr.opcode) {
            case aura::ir::IROpcode::ConstI64:
                known_[ops[0]] = (static_cast<std::int64_t>(ops[2]) << 32) | ops[1];
                break;
            case aura::ir::IROpcode::ConstF64:
                break;
            case aura::ir::IROpcode::Local: {
                auto it = known_.find(ops[1]);
                if (it != known_.end()) { replace(instr, ops[0], it->second); ++folded_; }
                break;
            }
# 128 "/home/dev/code/aura/src/compiler/pass_manager.ixx"
            case aura::ir::IROpcode::Add: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace(instr, ops[0], it_a->second + it_b->second); ++folded_; } break; }
            case aura::ir::IROpcode::Sub: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace(instr, ops[0], it_a->second - it_b->second); ++folded_; } break; }
            case aura::ir::IROpcode::Mul: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace(instr, ops[0], it_a->second * it_b->second); ++folded_; } break; }
            case aura::ir::IROpcode::Div: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace(instr, ops[0], it_a->second / it_b->second); ++folded_; } break; }
            case aura::ir::IROpcode::Eq: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace_bool(instr, ops[0], (it_a->second == it_b->second)); ++folded_; } break; }
            case aura::ir::IROpcode::Lt: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace_bool(instr, ops[0], (it_a->second < it_b->second)); ++folded_; } break; }
            case aura::ir::IROpcode::Gt: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace_bool(instr, ops[0], (it_a->second > it_b->second)); ++folded_; } break; }
            case aura::ir::IROpcode::Le: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace_bool(instr, ops[0], (it_a->second <= it_b->second)); ++folded_; } break; }
            case aura::ir::IROpcode::Ge: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace_bool(instr, ops[0], (it_a->second >= it_b->second)); ++folded_; } break; }
            case aura::ir::IROpcode::And: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace_bool(instr, ops[0], (it_a->second && it_b->second)); ++folded_; } break; }
            case aura::ir::IROpcode::Or: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace_bool(instr, ops[0], (it_a->second || it_b->second)); ++folded_; } break; }


            case aura::ir::IROpcode::Not: {
                auto it = known_.find(ops[1]);
                if (it != known_.end()) { replace_bool(instr, ops[0], !it->second); ++folded_; }
                break;
            }
            default: break;
            }
        }
    }

    void replace(aura::ir::IRInstruction& instr, std::uint32_t slot, std::int64_t val) {
        instr.opcode = aura::ir::IROpcode::ConstI64;
        instr.operands = {slot,
            static_cast<std::uint32_t>(val & 0xFFFFFFFF),
            static_cast<std::uint32_t>((val >> 32) & 0xFFFFFFFF), 0};
        known_[slot] = val;
    }

    void replace_bool(aura::ir::IRInstruction& instr, std::uint32_t slot, bool val) {
        instr.opcode = aura::ir::IROpcode::ConstBool;
        instr.operands = {slot, val ? 1u : 0u, 0, 0};
        known_[slot] = val ? 1 : 0;
    }

    std::unordered_map<std::uint32_t, std::int64_t> known_;
    std::size_t folded_ = 0;
};




export class TypeCheckWrap {
public:
    void run(aura::ir::IRModule& module) {


    }




    std::size_t check_before_lowering(
        aura::ast::FlatAST& flat,
        aura::ast::StringPool& pool,
        aura::ast::NodeId root,
        aura::core::TypeRegistry& type_registry,
        aura::diag::DiagnosticCollector& diag) {
        aura::compiler::TypeChecker tc(type_registry);
        tc.infer_flat(flat, pool, root, diag);
        auto all = diag.diagnostics();
        return all.size();
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "type-check"; }


    const std::vector<aura::diag::Diagnostic>& diagnostics() const {
        return last_diags_;
    }

private:
    std::vector<aura::diag::Diagnostic> last_diags_;
};
# 214 "/home/dev/code/aura/src/compiler/pass_manager.ixx"
export class TypeSpecializationWrap {
public:
    explicit TypeSpecializationWrap(const aura::core::TypeRegistry* reg = nullptr)
        : type_reg_(reg) {}

    void run(aura::ir::IRModule& module) {
        if (!type_reg_) return;
        auto dyn_id = type_reg_->lookup_type("Any");
        for (auto& func : module.functions) {
            for (auto& block : func.blocks) {
                std::size_t i = 0;
                while (i < block.instructions.size()) {
                    auto& instr = block.instructions[i];
                    auto& ops = instr.operands;




                    if (instr.opcode == aura::ir::IROpcode::Add ||
                        instr.opcode == aura::ir::IROpcode::Sub ||
                        instr.opcode == aura::ir::IROpcode::Mul ||
                        instr.opcode == aura::ir::IROpcode::Div) {
                        auto t1 = (ops[1] < block.instructions.size())
                            ? block.instructions[ops[1]].type_id : 0u;
                        auto t2 = (ops[2] < block.instructions.size())
                            ? block.instructions[ops[2]].type_id : 0u;

                        if (t1 != 0 && t2 != 0 && t1 != t2
            && t1 != dyn_id.index && t2 != dyn_id.index) {
                            auto cast_slot = func.local_count++;
                            aura::ir::IRInstruction cast_instr;
                            cast_instr.opcode = aura::ir::IROpcode::CastOp;
                            cast_instr.operands = {cast_slot, ops[2], 0, 0};
                            cast_instr.type_id = t1;
                            block.instructions.insert(
                                block.instructions.begin() + static_cast<std::ptrdiff_t>(i),
                                cast_instr);
                            ++i;
                            ops[2] = cast_slot;
                        }
                        ++i;
                        continue;
                    }


                    if (instr.opcode == aura::ir::IROpcode::CastOp && ops[2] == 3) {
                        auto source_type = (ops[1] < block.instructions.size())
                            ? block.instructions[ops[1]].type_id : 0u;
                        if (source_type != 0 && source_type == instr.type_id) {
                            block.instructions[i].opcode = aura::ir::IROpcode::Local;
                            block.instructions[i].operands = {ops[0], ops[1], 0, 0};
                        }
                    }

                    ++i;
                }
            }
        }
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "type-specialize"; }
    std::size_t specialized_count() const { return removed_count_; }

private:
    const aura::core::TypeRegistry* type_reg_ = nullptr;
    std::size_t removed_count_ = 0;
};

}
