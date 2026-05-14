# 0 "/home/dev/code/aura/src/compiler/pass_manager.ixx"
# 1 "/home/dev/code/aura/build_debug//"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/compiler/pass_manager.ixx"
export module aura.compiler.pass_manager;
import std;
import aura.compiler.ir;
import aura.compiler.compute_kind;
import aura.compiler.arity;

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
            case aura::ir::IROpcode::Local: {
                auto it = known_.find(ops[1]);
                if (it != known_.end()) { replace(instr, ops[0], it->second); ++folded_; }
                break;
            }







            case aura::ir::IROpcode::Add: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace(instr, ops[0], it_a->second + it_b->second); ++folded_; } break; }
            case aura::ir::IROpcode::Sub: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace(instr, ops[0], it_a->second - it_b->second); ++folded_; } break; }
            case aura::ir::IROpcode::Mul: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace(instr, ops[0], it_a->second * it_b->second); ++folded_; } break; }
            case aura::ir::IROpcode::Div: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace(instr, ops[0], it_a->second / it_b->second); ++folded_; } break; }
            case aura::ir::IROpcode::Eq: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace(instr, ops[0], static_cast<std::int64_t>(it_a->second == it_b->second)); ++folded_; } break; }
            case aura::ir::IROpcode::Lt: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace(instr, ops[0], static_cast<std::int64_t>(it_a->second < it_b->second)); ++folded_; } break; }
            case aura::ir::IROpcode::Gt: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace(instr, ops[0], static_cast<std::int64_t>(it_a->second > it_b->second)); ++folded_; } break; }
            case aura::ir::IROpcode::Le: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace(instr, ops[0], static_cast<std::int64_t>(it_a->second <= it_b->second)); ++folded_; } break; }
            case aura::ir::IROpcode::Ge: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace(instr, ops[0], static_cast<std::int64_t>(it_a->second >= it_b->second)); ++folded_; } break; }
            case aura::ir::IROpcode::And: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace(instr, ops[0], static_cast<std::int64_t>(it_a->second && it_b->second)); ++folded_; } break; }
            case aura::ir::IROpcode::Or: { auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]); if (it_a != known_.end() && it_b != known_.end()) { replace(instr, ops[0], static_cast<std::int64_t>(it_a->second || it_b->second)); ++folded_; } break; }

            case aura::ir::IROpcode::Not: {
                auto it = known_.find(ops[1]);
                if (it != known_.end()) { replace(instr, ops[0], !it->second); ++folded_; }
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

    std::unordered_map<std::uint32_t, std::int64_t> known_;
    std::size_t folded_ = 0;
};

}
