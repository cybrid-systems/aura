export module aura.compiler.pass_manager;
import std;
import aura.compiler.ir;
import aura.compiler.compute_kind;
import aura.compiler.arity;

namespace aura::compiler {

// ── Pass concept — any type with run(IRModule&) + has_error() ───
export template <typename P>
concept Pass = requires(P& p, aura::ir::IRModule& m) {
    { p.run(m) } -> std::same_as<void>;
    { p.has_error() } -> std::convertible_to<bool>;
};

// ── run_pipeline — fold over passes with short-circuit ──────────
export template <Pass... Passes>
bool run_pipeline(aura::ir::IRModule& mod, Passes&... passes) {
    return (run_one(mod, passes) && ...);
}

// ── run_one — execute a single pass, return true if no error ────
export template <Pass P>
bool run_one(aura::ir::IRModule& mod, P& pass) {
    pass.run(mod);
    return !pass.has_error();
}

// ── ComputeKindWrap — analysis pass (wraps pure function) ─────
export class ComputeKindWrap {
public:
    void run(aura::ir::IRModule& module) {
        results_.clear();
        for (auto& func : module.functions)
            results_.push_back(aura::compiler::compute_kind(func));
    }

    // Phase 4: per-function analysis — cleanly supports incremental compilation
    ComputeKindResult compute_function(const aura::ir::IRFunction& func) {
        return aura::compiler::compute_kind(func);
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "compute-kind"; }
    const std::vector<ComputeKindResult>& results() const { return results_; }

private:
    std::vector<ComputeKindResult> results_;
};

// ── ArityWrap — arity checking pass ────────────────────────────
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

// ── ConstantFoldingWrap — compile-time constant folding ─────────
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

    // Phase 4: per-function constant folding — reuses the private fold_block logic.
    // Each function's blocks are folded independently; known_ is reset per block.
    // Returns the number of instructions folded in this function.
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
#define FOLD_BIN(OP, EXPR)                                                    \
    case aura::ir::IROpcode::OP: {                                            \
        auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]);         \
        if (it_a != known_.end() && it_b != known_.end()) {                   \
            replace(instr, ops[0], EXPR); ++folded_;                          \
        } break;                                                              \
    }
            FOLD_BIN(Add, it_a->second + it_b->second)
            FOLD_BIN(Sub, it_a->second - it_b->second)
            FOLD_BIN(Mul, it_a->second * it_b->second)
            FOLD_BIN(Div, it_a->second / it_b->second)
            FOLD_BIN(Eq,  static_cast<std::int64_t>(it_a->second == it_b->second))
            FOLD_BIN(Lt,  static_cast<std::int64_t>(it_a->second < it_b->second))
            FOLD_BIN(Gt,  static_cast<std::int64_t>(it_a->second > it_b->second))
            FOLD_BIN(Le,  static_cast<std::int64_t>(it_a->second <= it_b->second))
            FOLD_BIN(Ge,  static_cast<std::int64_t>(it_a->second >= it_b->second))
            FOLD_BIN(And, static_cast<std::int64_t>(it_a->second && it_b->second))
            FOLD_BIN(Or,  static_cast<std::int64_t>(it_a->second || it_b->second))
#undef FOLD_BIN
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

} // namespace aura::compiler
