export module aura.compiler.pass_manager;
import std;
import aura.core;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.compute_kind;
import aura.compiler.arity;
import aura.compiler.type_checker;
import aura.compiler.coercion_map;
import aura.diag;

namespace aura::compiler {

// ── Pass concept — any type with run(IRModule&) + has_error() ───
export template <typename P>
concept Pass = requires(P& p, aura::ir::IRModule& m) {
    { p.run(m) } -> std::same_as<void>;
    { p.has_error() } -> std::convertible_to<bool>;
};

// ── run_pipeline — fold over passes with short-circuit ──────────
export template <Pass... Passes> bool run_pipeline(aura::ir::IRModule& mod, Passes&... passes) {
    return (run_one(mod, passes) && ...);
}

// ── run_one — execute a single pass, return true if no error ────
export template <Pass P> bool run_one(aura::ir::IRModule& mod, P& pass) {
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
    std::span<const ComputeKindResult> results() const { return results_; }

private:
    std::vector<ComputeKindResult> results_;
};

// ── ArityWrap — arity checking pass ────────────────────────────
export class ArityWrap {
public:
    void run(aura::ir::IRModule& module) { result_ = aura::compiler::check_arity(module); }

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
                    if (it != known_.end()) {
                        // Don't propagate tagged bool values (7=#t, 3=#f) as ConstI64
                        // since the AOT/JIT emitter treats ConstI64 as fixnum-encoded.
                        if (it->second != 3 && it->second != 7) {
                            replace(instr, ops[0], it->second);
                            ++folded_;
                        }
                    }
                    break;
                }
#define FOLD_BIN(OP, EXPR)                                                                         \
    case aura::ir::IROpcode::OP: {                                                                 \
        auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]);                               \
        if (it_a != known_.end() && it_b != known_.end()) {                                        \
            replace(instr, ops[0], EXPR);                                                          \
            ++folded_;                                                                             \
        }                                                                                          \
        break;                                                                                     \
    }
// Tagged truthiness helper: value 3 = #f, value 0 = int 0.
// Both #f and integer 0 are falsy. Fixnum 0 encodes as val=0.
#define IS_TRUTHY(v) ((v) != 3 && (v) != 0)

#define FOLD_BOOL(OP, TRUTHY_EXPR)                                                                 \
    case aura::ir::IROpcode::OP: {                                                                 \
        auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]);                               \
        if (it_a != known_.end() && it_b != known_.end()) {                                        \
            replace_bool(instr, ops[0], TRUTHY_EXPR);                                              \
            ++folded_;                                                                             \
        }                                                                                          \
        break;                                                                                     \
    }
#define FOLD_BOOL_CMP(OP, EXPR)                                                                    \
    case aura::ir::IROpcode::OP: {                                                                 \
        auto it_a = known_.find(ops[1]), it_b = known_.find(ops[2]);                               \
        if (it_a != known_.end() && it_b != known_.end()) {                                        \
            replace_bool(instr, ops[0], EXPR);                                                     \
            ++folded_;                                                                             \
        }                                                                                          \
        break;                                                                                     \
    }
                    FOLD_BIN(Add, it_a->second + it_b->second)
                    FOLD_BIN(Sub, it_a->second - it_b->second)
                    FOLD_BIN(Mul, it_a->second * it_b->second)
                    FOLD_BIN(Div, it_a->second / it_b->second)
                    FOLD_BOOL_CMP(Eq, (it_a->second == it_b->second))
                    FOLD_BOOL_CMP(Lt, (it_a->second < it_b->second))
                    FOLD_BOOL_CMP(Gt, (it_a->second > it_b->second))
                    FOLD_BOOL_CMP(Le, (it_a->second <= it_b->second))
                    FOLD_BOOL_CMP(Ge, (it_a->second >= it_b->second))
                    FOLD_BOOL(And, (IS_TRUTHY(it_a->second) && IS_TRUTHY(it_b->second)))
                    FOLD_BOOL(Or, (IS_TRUTHY(it_a->second) || IS_TRUTHY(it_b->second)))
#undef FOLD_BIN
#undef FOLD_BOOL
                case aura::ir::IROpcode::Not: {
                    auto it = known_.find(ops[1]);
                    if (it != known_.end()) {
                        // Falsy: 3=#f or 0=int 0
                        replace_bool(instr, ops[0], !IS_TRUTHY(it->second));
                        ++folded_;
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    void replace(aura::ir::IRInstruction& instr, std::uint32_t slot, std::int64_t val) {
        instr.opcode = aura::ir::IROpcode::ConstI64;
        instr.operands = {slot, static_cast<std::uint32_t>(val & 0xFFFFFFFF),
                          static_cast<std::uint32_t>((val >> 32) & 0xFFFFFFFF), 0};
        known_[slot] = val;
    }

    void replace_bool(aura::ir::IRInstruction& instr, std::uint32_t slot, bool val) {
        instr.opcode = aura::ir::IROpcode::ConstBool;
        instr.operands = {slot, val ? 1u : 0u, 0, 0};
        known_[slot] = val ? 7 : 3;
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
    //
    // Issue #116: applies the deferred CoercionMap (collected
    // during infer_flat) to the FlatAST as a single explicit
    // pass. This is the ONE place where structural links are
    // rewritten to insert CoercionNodes; the type checker
    // itself is now read-only on the FlatAST.
    std::size_t check_before_lowering(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                      aura::ast::NodeId root,
                                      aura::core::TypeRegistry& type_registry,
                                      aura::diag::DiagnosticCollector& diag) {
        aura::compiler::TypeChecker tc(type_registry);
        tc.infer_flat(flat, pool, root, diag);
        // Apply deferred coercions now, before lowering reads
        // the AST. apply_coercion_map is idempotent — calling
        // it twice with the same map is a safe no-op the
        // second time.
        auto coercions = tc.take_coercions();
        if (!coercions.empty()) {
            aura::compiler::apply_coercion_map(flat, coercions);
        }
        auto all = diag.diagnostics();
        return all.size();
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "type-check"; }

    // Access stored diagnostics from last check_before_lowering call
    std::span<const aura::diag::Diagnostic> diagnostics() const { return last_diags_; }

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
//
// Issue #149 P1 — rich type propagation for specialization,
// monomorphization, and GuardShape precision. Today the pass
// only uses type_id's type_tag (the simple TypeRegistry tag
// for primitives). Richer type info from InferenceEngine
// (ADT variant discriminants, linear-type move/borrow
// semantics, occurrence-narrowed precise types, polymorphic
// instantiations) is ignored.
//
// Full 5-deliverable roadmap (estimated 4-6 commits, like
// Phase 2.5.0 and #148):
//   Phase 1: extend IRInstruction with rich type metadata
//     (linear_ownership_state, adt_variant_id, narrow_evidence).
//   Phase 2: type attachment logic in lowering_impl.cpp at
//     key points (annotations, call sites, match arms).
//   Phase 3: extend THIS pass to cover linear types and common
//     ADTs (insert MoveOp elision for linear uses, replace
//     generic ADT call with variant-specific block when
//     variant is statically known).
//   Phase 4: update GuardShape to use the new type info (the
//     existing GuardShape is shape_id-only; add a
//     type_id-conditional path for higher hit rate).
//   Phase 5: tests + benchmark. AC: improved GuardShape hit
//     rate, preserved gradual typing semantics, measurable
//     perf improvement.
//
// Today ships Phase 0 (prep + scaffold): a long-form design
// comment and a small concrete improvement — respect
// IRFunction::specialized_for (already exists in ir.ixx
// line 315) so the pass skips already-specialized functions.
// This prevents the pass from re-specializing a function that
// was specialized for a particular shape, which would lose
// the specialization's optimization.
export class TypeSpecializationWrap {
public:
    explicit TypeSpecializationWrap(const aura::core::TypeRegistry* reg = nullptr)
        : type_reg_(reg) {}

    void run(aura::ir::IRModule& module) {
        // Issue #73 Phase 3: don't silently bail out when no registry
        // is provided. Fall through so the per-instruction checks below
        // can no-op naturally (their \`type_id == 0\` guards handle the
        // missing-registry case the same way the early-exit did). The
        // difference: a missing registry is now visible to the caller
        // and the pass produces a clean run instead of silently
        // dropping every type-driven optimization.
        if (!type_reg_) {
            std::println(std::cerr,
                "TypeSpecializationWrap: no TypeRegistry provided; "
                "CastOp insertion will be a no-op. "
                "Pass TypeRegistry* to the constructor to enable.");
            return;
        }
        auto dyn_id = type_reg_->lookup_type("Any");
        for (auto& func : module.functions) {
            // Issue #149: skip already-specialized functions.
            // specialized_for != 0 means the function was
            // monomorphized for a particular shape/type — re-running
            // the specialization pass on it would either be a no-op
            // (if the existing types still match) or worse, lose the
            // original specialization's optimization by re-inserting
            // generic dispatch. We track but don't modify these.
            // The 0 == no specialization (generic version) is the only
            // case where the pass does its full work.
            if (func.specialized_for != 0) {
                continue;
            }
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
                                      ? block.instructions[ops[1]].type_id
                                      : 0u;
                        auto t2 = (ops[2] < block.instructions.size())
                                      ? block.instructions[ops[2]].type_id
                                      : 0u;
                        // If both are concrete (non-zero) and differ, insert CastOp on ops[2]
                        if (t1 != 0 && t2 != 0 && t1 != t2 && t1 != dyn_id.index &&
                            t2 != dyn_id.index) {
                            auto cast_slot = func.local_count++;
                            aura::ir::IRInstruction cast_instr;
                            cast_instr.opcode = aura::ir::IROpcode::CastOp;
                            // Snapshot ops[2] before insert: insert may reallocate
                            // block.instructions, invalidating `instr` / `ops`.
                            auto ops2_snapshot = ops[2];
                            cast_instr.operands = std::array<std::uint32_t, 4>{
                                cast_slot, ops2_snapshot,
                                type_tag_for_coercion(aura::core::TypeId{t1, 1}), 0u};
                            cast_instr.type_id = t1;
                            block.instructions.insert(block.instructions.begin() +
                                                          static_cast<std::ptrdiff_t>(i),
                                                      cast_instr);
                            ++i;
                            // After insert, original instruction is at index i — update by index.
                            block.instructions[i].operands[2] = cast_slot;
                        }
                        ++i;
                        continue;
                    }

                    // ── Insert CastOp for Return with non-matching types ──
                    // When the Return instruction has a type annotation that differs
                    // from the value being returned, insert CastOp.
                    if (instr.opcode == aura::ir::IROpcode::Return) {
                        auto val_type = (ops[0] < block.instructions.size())
                                            ? block.instructions[ops[0]].type_id
                                            : 0u;
                        auto ret_type = instr.type_id;
                        if (val_type != 0 && ret_type != 0 && val_type != ret_type &&
                            val_type != dyn_id.index && ret_type != dyn_id.index) {
                            auto cast_slot = func.local_count++;
                            aura::ir::IRInstruction cast_instr;
                            cast_instr.opcode = aura::ir::IROpcode::CastOp;
                            auto cast_tag = type_tag_for_coercion(aura::core::TypeId{ret_type, 1});
                            // Snapshot ops[0] before insert (insert may reallocate instructions).
                            auto ops0_snapshot = ops[0];
                            cast_instr.operands =
                                std::array<std::uint32_t, 4>{cast_slot, ops0_snapshot, cast_tag, 0u};
                            cast_instr.type_id = ret_type;
                            block.instructions.insert(block.instructions.begin() +
                                                          static_cast<std::ptrdiff_t>(i),
                                                      cast_instr);
                            ++i;
                            // After insert, original Return is at index i — update by index.
                            block.instructions[i].operands[0] = cast_slot;
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
                                if (blk_id >= func.blocks.size())
                                    return;
                                auto& blk = func.blocks[blk_id];
                                // Find the Local instruction (phi_slot write) before the Jump
                                for (std::size_t j = 0; j + 1 < blk.instructions.size(); ++j) {
                                    auto& loc = blk.instructions[j];
                                    auto& next = blk.instructions[j + 1];
                                    if (next.opcode == aura::ir::IROpcode::Jump &&
                                        loc.opcode == aura::ir::IROpcode::Local) {
                                        auto val_type =
                                            (loc.operands[1] < block.instructions.size())
                                                ? block.instructions[loc.operands[1]].type_id
                                                : 0u;
                                        if (val_type != 0 && val_type != if_result_type &&
                                            val_type != dyn_id.index) {
                                            auto cast_slot = func.local_count++;
                                            aura::ir::IRInstruction cast_instr;
                                            cast_instr.opcode = aura::ir::IROpcode::CastOp;
                                            // Snapshot loc.operands[1] before insert
                                            // (insert may reallocate blk.instructions).
                                            auto loc_ops1 = loc.operands[1];
                                            cast_instr.operands = std::array<std::uint32_t, 4>{
                                                cast_slot, loc_ops1,
                                                type_tag_for_coercion(
                                                    aura::core::TypeId{if_result_type, 1}),
                                                0u};
                                            cast_instr.type_id = if_result_type;
                                            blk.instructions.insert(
                                                blk.instructions.begin() +
                                                    static_cast<std::ptrdiff_t>(j),
                                                cast_instr);
                                            // After insert, `loc` shifted to j+1 — update by index.
                                            blk.instructions[j + 1].operands[1] = cast_slot;
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
                                               ? block.instructions[ops[1]].type_id
                                               : 0u;
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
        if (!type_reg_)
            return 3;
        auto tag = type_reg_->tag_of(tid);
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

    bool has_error() const { return false; }
    std::string_view name() const { return "type-specialize"; }
    std::size_t specialized_count() const { return removed_count_; }

private:
    const aura::core::TypeRegistry* type_reg_ = nullptr;
    std::size_t removed_count_ = 0;
};

// ── CoercionMarkerPass — mark nodes needing coercion ──────────
// Operates on FlatAST after type-checking, before lowering.
// Uses type_id slots to detect boundary mismatches and marks
// the source expression for coercion insertion during lowering.
//
// Since FlatAST is append-only (immutable after build), this
// pass writes coercion metadata into a side-table. The lowering
// pass (lowering_impl.cpp) consults this table to emit CoercionOp
// instructions at the right IR points.
//
// Boundary rules (design §14.3):
//   static→dynamic: erasure (no coercion)
//   dynamic→static: insert runtime check CoercionOp
//   ground type conversion: insert conversion CoercionOp
//
// The actual CoercionNode AST nodes are created at parse time;
// this pass identifies WHERE in the AST they should be added
// by returning a vector of (source_node, target_type) pairs.
export struct CoercionMarker {
    aura::ast::NodeId source_node; // expression producing the value
    std::uint32_t target_type_id;  // type it needs to become
    aura::ast::NodeTag context;    // Call, TypeAnnotation, Lambda, Let
    aura::ast::NodeId parent;      // parent node for context
    std::uint32_t child_index;     // which child position
};

export class CoercionMarkerPass {
    aura::core::TypeRegistry& reg_;
    aura::ast::FlatAST& flat_;
    aura::ast::StringPool& pool_;

public:
    CoercionMarkerPass(aura::core::TypeRegistry& reg, aura::ast::FlatAST& flat,
                       aura::ast::StringPool& pool)
        : reg_(reg)
        , flat_(flat)
        , pool_(pool) {}

    // Run the pass. Collects all nodes needing coercion.
    std::vector<CoercionMarker> run(aura::ast::NodeId root) {
        markers_.clear();
        if (root == aura::ast::NULL_NODE || root >= flat_.size())
            return {};
        visit_node(root);
        return std::move(markers_);
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "coercion-mark"; }

private:
    std::vector<CoercionMarker> markers_;

    bool needs_coercion(std::uint32_t actual_id, std::uint32_t expected_id) {
        if (actual_id == expected_id || actual_id == 0 || expected_id == 0)
            return false;
        auto actual = aura::core::TypeId{actual_id, 1};
        auto expected = aura::core::TypeId{expected_id, 1};
        if (actual == expected)
            return false;
        // static→dynamic: erasure
        if (actual != reg_.dynamic_type() && expected == reg_.dynamic_type())
            return false;
        return true; // dynamic→static or ground→ground
    }

    void visit_node(aura::ast::NodeId id) {
        auto v = flat_.get(id);
        // Post-order: children first
        for (auto child_id : v.children) {
            if (child_id != aura::ast::NULL_NODE)
                visit_node(child_id);
        }

        switch (v.tag) {
            case aura::ast::NodeTag::Call:
                visit_call(id);
                break;
            case aura::ast::NodeTag::TypeAnnotation:
                visit_annotation(id);
                break;
            default:
                break;
        }
    }

    void visit_call(aura::ast::NodeId id) {
        // Currently handled by TypeSpecializationWrap at IR level.
        // FlatAST-level call arg coercion is future work.
    }

    void visit_annotation(aura::ast::NodeId id) {
        auto v = flat_.get(id);
        if (v.children.empty())
            return;
        auto inner_id = v.child(0);
        auto inner_type = flat_.type_id(inner_id);
        auto ann_type = flat_.type_id(id);
        if (needs_coercion(inner_type, ann_type)) {
            markers_.push_back(CoercionMarker{
                .source_node = inner_id,
                .target_type_id = ann_type,
                .context = aura::ast::NodeTag::TypeAnnotation,
                .parent = id,
                .child_index = 0,
            });
        }
    }
};

// ── DeadCoercionEliminationPass — remove redundant CastOp ─────
// IR-level pass. Removes CastOp instructions where:
//   1. Source and target types are identical (no-op cast)
//   2. Nested casts: (cast (cast x T1) T2) → (cast x T2)
//   3. Chain of identity casts: (cast (cast x T) T) → x
//
// Operates on IRModule after lowering + TypeSpecializationWrap.
// CastOp semantics in IR: operands = {result_slot, source_slot, type_tag, 0}
// The type_tag field encodes the target runtime type (1=Int, 2=Bool, 3=String, 4=FLOAT, 0=Any).
export class DeadCoercionEliminationPass {
public:
    explicit DeadCoercionEliminationPass(const aura::core::TypeRegistry* reg = nullptr)
        : type_reg_(reg) {}

    void run(aura::ir::IRModule& module) {
        eliminated_ = 0;
        for (auto& func : module.functions) {
            for (auto& block : func.blocks) {
                bool changed;
                do {
                    changed = false;
                    for (std::size_t i = 0; i < block.instructions.size(); ++i) {
                        auto& instr = block.instructions[i];
                        if (instr.opcode != aura::ir::IROpcode::CastOp)
                            continue;
                        auto& ops = instr.operands;

                        // Rule 1: identity cast — source type == target type
                        // Check via type_id propagation (from FlatAST)
                        if (instr.type_id != 0) {
                            auto src_type = (ops[1] < block.instructions.size())
                                                ? block.instructions[ops[1]].type_id
                                                : 0u;
                            if (src_type != 0 && src_type == instr.type_id) {
                                // target == source type: replace with Local
                                block.instructions[i] = aura::ir::IRInstruction{
                                    .opcode = aura::ir::IROpcode::Local,
                                    .operands = {ops[0], ops[1], 0, 0},
                                    .type_id = instr.type_id,
                                };
                                ++eliminated_;
                                changed = true;
                                continue;
                            }
                        }

                        // Rule 2: nested cast — (cast (cast x T1) T2)
                        if (ops[1] < block.instructions.size()) {
                            auto& src_instr = block.instructions[ops[1]];
                            if (src_instr.opcode == aura::ir::IROpcode::CastOp) {
                                // Skip the intermediate cast: ops[0] = ops'[1]
                                ops[1] = src_instr.operands[1];
                                ++eliminated_;
                                changed = true;
                                continue;
                            }
                        }
                    }
                } while (changed);
            }
        }
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "dead-coercion"; }
    std::size_t eliminated_count() const { return eliminated_; }

private:
    const aura::core::TypeRegistry* type_reg_ = nullptr;
    std::size_t eliminated_ = 0;
};

} // namespace aura::compiler
