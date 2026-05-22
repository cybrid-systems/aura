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
#define FOLD_BOOL(OP, EXPR)                                                   \
    case aura::ir::IROpcode::OP: {                                            \
        auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]);         \
        if (it_a != known_.end() && it_b != known_.end()) {                   \
            replace_bool(instr, ops[0], EXPR); ++folded_;                     \
        } break;                                                              \
    }
            FOLD_BIN(Add, it_a->second + it_b->second)
            FOLD_BIN(Sub, it_a->second - it_b->second)
            FOLD_BIN(Mul, it_a->second * it_b->second)
            FOLD_BIN(Div, it_a->second / it_b->second)
            FOLD_BOOL(Eq, (it_a->second == it_b->second))
            FOLD_BOOL(Lt, (it_a->second < it_b->second))
            FOLD_BOOL(Gt, (it_a->second > it_b->second))
            FOLD_BOOL(Le, (it_a->second <= it_b->second))
            FOLD_BOOL(Ge, (it_a->second >= it_b->second))
            FOLD_BOOL(And, (it_a->second && it_b->second))
            FOLD_BOOL(Or, (it_a->second || it_b->second))
#undef FOLD_BIN
#undef FOLD_BOOL
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

// ── TypeCheckWrap — type checking pass (pre-lowering, FlatAST level) ──
// Unlike other passes, this operates on FlatAST before IR lowering.
// The run(IRModule&) is a no-op; the real work is in check_before_lowering().
export class TypeCheckWrap {
public:
    void run(aura::ir::IRModule& module) {
        // Type check is FlatAST-level, not IRModule-level.
        // Use check_before_lowering() for the actual work.
    }

    // Run type checking on FlatAST before lowering.
    // Returns the number of type errors found (0 = clean).
    // Diagnostics are collected in diag for optional reporting.
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

    // Access stored diagnostics from last check_before_lowering call
    const std::vector<aura::diag::Diagnostic>& diagnostics() const {
        return last_diags_;
    }

private:
    std::vector<aura::diag::Diagnostic> last_diags_;
};

// ── TypeSpecializationWrap — type-aware IR pass ────────────────
// Operates on IRModule after lowering, using type_id fields on instructions.
// Can:
//   1. Insert CastOp when arithmetic operands have non-matching concrete types
//   2. Remove redundant CastOp (coercing type to itself)
//   3. Annotate instructions with inferred result types from operands
//
// Relies on type_ids being propagated from FlatAST via lowering.
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

                    // ── Insert CastOp for Add/Sub/Mul/Div with non-matching types ──
                    // If both operands have known concrete type_ids and they differ,
                    // insert CastOp to coerce the second operand to match the first.
                    if (instr.opcode == aura::ir::IROpcode::Add ||
                        instr.opcode == aura::ir::IROpcode::Sub ||
                        instr.opcode == aura::ir::IROpcode::Mul ||
                        instr.opcode == aura::ir::IROpcode::Div) {
                        auto t1 = (ops[1] < block.instructions.size())
                            ? block.instructions[ops[1]].type_id : 0u;
                        auto t2 = (ops[2] < block.instructions.size())
                            ? block.instructions[ops[2]].type_id : 0u;
                        // If both are concrete (non-zero) and differ, insert CastOp on ops[2]
                        if (t1 != 0 && t2 != 0 && t1 != t2
            && t1 != dyn_id.index && t2 != dyn_id.index) {
                            auto cast_slot = func.local_count++;
                            aura::ir::IRInstruction cast_instr;
                            cast_instr.opcode = aura::ir::IROpcode::CastOp;
                            cast_instr.operands = std::array<std::uint32_t, 4>{cast_slot, ops[2], type_tag_for_coercion(aura::core::TypeId{t1, 1}), 0u};
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

                    // ── Insert CastOp for Return with non-matching types ──
                    // When the Return instruction has a type annotation that differs
                    // from the value being returned, insert CastOp.
                    if (instr.opcode == aura::ir::IROpcode::Return) {
                        auto val_type = (ops[0] < block.instructions.size())
                            ? block.instructions[ops[0]].type_id : 0u;
                        auto ret_type = instr.type_id;
                        if (val_type != 0 && ret_type != 0 && val_type != ret_type
                            && val_type != dyn_id.index && ret_type != dyn_id.index) {
                            auto cast_slot = func.local_count++;
                            aura::ir::IRInstruction cast_instr;
                            cast_instr.opcode = aura::ir::IROpcode::CastOp;
                            auto cast_tag = type_tag_for_coercion(aura::core::TypeId{ret_type, 1});
                            cast_instr.operands = std::array<std::uint32_t, 4>{cast_slot, ops[0], cast_tag, 0u};
                            cast_instr.type_id = ret_type;
                            block.instructions.insert(
                                block.instructions.begin() + static_cast<std::ptrdiff_t>(i),
                                cast_instr);
                            ++i;
                            ops[0] = cast_slot;
                        }
                        ++i;
                        continue;
                    }

                    // ── Insert CastOp for if branches (phi_slot type mismatch) ──
                    // If expressions emit Branch cond, then_blk, else_blk; the result is written
                    // to a phi_slot via Local in each branch. If the Branch has a concrete
                    // type_id (from the if expression's inference result), check that both
                    // branch values match that type.
                    if (instr.opcode == aura::ir::IROpcode::Branch) {
                        auto if_result_type = instr.type_id;
                        if (if_result_type != 0 && if_result_type != dyn_id.index) {
                            auto then_blk = ops[1];
                            auto else_blk = ops[2];
                            auto check_and_cast = [&](std::uint32_t blk_id) {
                                if (blk_id >= func.blocks.size()) return;
                                auto& blk = func.blocks[blk_id];
                                // Find the Local instruction (phi_slot write) before the Jump
                                for (std::size_t j = 0; j + 1 < blk.instructions.size(); ++j) {
                                    auto& loc = blk.instructions[j];
                                    auto& next = blk.instructions[j + 1];
                                    if (next.opcode == aura::ir::IROpcode::Jump &&
                                        loc.opcode == aura::ir::IROpcode::Local) {
                                        auto val_type = (loc.operands[1] < block.instructions.size())
                                            ? block.instructions[loc.operands[1]].type_id : 0u;
                                        if (val_type != 0 && val_type != if_result_type
                                            && val_type != dyn_id.index) {
                                            auto cast_slot = func.local_count++;
                                            aura::ir::IRInstruction cast_instr;
                                            cast_instr.opcode = aura::ir::IROpcode::CastOp;
                                            cast_instr.operands = std::array<std::uint32_t, 4>{
                                                cast_slot, loc.operands[1],
                                                type_tag_for_coercion(aura::core::TypeId{if_result_type, 1}), 0u};
                                            cast_instr.type_id = if_result_type;
                                            blk.instructions.insert(
                                                blk.instructions.begin() + static_cast<std::ptrdiff_t>(j),
                                                cast_instr);
                                            loc.operands[1] = cast_slot;
                                        }
                                        break;
                                    }
                                }
                            };
                            check_and_cast(then_blk);
                            check_and_cast(else_blk);
                        }
                        ++i;
                        continue;
                    }

                    // ── Remove redundant CastOp ──
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

    // Map TypeId to CastOp type_tag (used by IR interpreter)
    // INT→0, STRING→1, BOOL→2, FLOAT→4, DYNAMIC→3
    std::uint32_t type_tag_for_coercion(aura::core::TypeId tid) const {
        if (!type_reg_) return 3;
        auto tag = type_reg_->tag_of(tid);
        switch (tag) {
            case aura::core::TypeTag::INT:    return 0;
            case aura::core::TypeTag::STRING: return 1;
            case aura::core::TypeTag::BOOL:   return 2;
            case aura::core::TypeTag::FLOAT:  return 4;
            default:                          return 3; // Dynamic / pass-through
        }
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "type-specialize"; }
    std::size_t specialized_count() const { return removed_count_; }

private:
    const aura::core::TypeRegistry* type_reg_ = nullptr;
    std::size_t removed_count_ = 0;
};

} // namespace aura::compiler
