export module aura.compiler.lowering;
import std;
import aura.core;
import aura.diag;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.evaluator;

namespace aura::compiler {

// A slot binding: either a local variable (slot index) or captured (env slot)
enum class BindingKind : std::uint8_t { Local, Captured, Cell };

struct Binding {
    BindingKind kind;
    std::uint32_t slot;
};

// ── LoweringState — all mutable lowering state ────────────────
// Issue #133: exported so the new lowering_linear_types
// module (and future extracted lowering modules) can
// operate on the same state without re-declaring the
// shape. The struct holds mutable lowering context:
// the module being built, the current function/block,
// local variable counter, region, type-registry /
// primitives / cache pointers.
export struct LoweringState {
    ast::ASTArena& arena;
    aura::ir::IRModule module;
    aura::ir::IRFunction* cur_func = nullptr;
    std::uint32_t cur_block = 0;
    std::uint32_t local_count = 0;
    std::vector<std::unordered_map<std::string, Binding>> scopes;
    std::uint32_t env_slot = 0;
    std::unordered_map<std::string, std::uint32_t> free_var_map;
    std::unordered_set<std::string> cell_free_vars;
    const Primitives* primitives = nullptr;     // for loading primitive values
    const ast::FlatAST* current_flat = nullptr; // for closure bridge data
    const ast::StringPool* current_pool = nullptr;
    const std::unordered_map<std::string, std::vector<aura::ir::ClosureBridgeData>>* cache_bridge =
        nullptr;
    const std::unordered_map<std::string, std::vector<std::string>>* cache_strings = nullptr;

    // Self-reference support: function name and its pre-allocated func_id
    // Used by cached define functions to emit correct MakeClosure for self-recursion.
    std::string self_name;
    std::uint32_t self_func_id = 0;

    // Current source AST node being lowered (for type propagation to IR)
    ast::NodeId current_source_id = ast::NULL_NODE;

    aura::ir::Region region = aura::ir::Region::Default;

    // Optional type registry for call-site coercion (P0 call boundary)
    const aura::core::TypeRegistry* type_reg = nullptr;

    // RAII scope guard: saves/restores current_source_id.
    // Place at the top of lower_flat_expr so child processing
    // doesn't overwrite the parent's source node for the result emit.
    struct SourceScope {
        LoweringState& state;
        ast::NodeId saved;
        SourceScope(LoweringState& s, ast::NodeId id)
            : state(s), saved(s.current_source_id) {
            state.current_source_id = id;
        }
        ~SourceScope() { state.current_source_id = saved; }
        SourceScope(const SourceScope&) = delete;
        SourceScope& operator=(const SourceScope&) = delete;
    };

    explicit LoweringState(ast::ASTArena& a)
        : arena(a) {}
    std::uint32_t alloc_local() { return local_count++; }

    // Emit with explicit type_id override. Use when the result type
    // differs from current_source_id (e.g., CastOp target type).
    void emit_with_type(aura::ir::IROpcode op, std::uint32_t tid,
                        std::uint32_t op0, std::uint32_t op1 = 0,
                        std::uint32_t op2 = 0, std::uint32_t op3 = 0) {
        emit(op, op0, op1, op2, op3);
        if (cur_func && cur_block < cur_func->blocks.size() && tid != 0) {
            cur_func->blocks[cur_block].instructions.back().type_id = tid;
        }
    }

    // Issue #149 Phase 2: emit with rich type metadata. Sets
    // type_id, linear_ownership_state, adt_variant_id, and
    // narrow_evidence on the last instruction in one call.
    // All four parameters default to 0 (= unknown / not set),
    // so callers that don't have the info just call
    // emit() / emit_with_type() as before. When Phase 3
    // (ADT/linear specialization) starts populating these,
    // the relevant emit sites will switch to this helper.
    //
    // Use:
    //   - linear_state: 0=untracked, 1=Owned, 2=Borrowed,
    //     3=MutBorrowed, 4=Moved (M4 Linear typing).
    //   - adt_variant: 0=not ADT, non-zero=discriminant
    //     (Phase 3 will populate from type info).
    //   - narrow_evidence: 0=no narrowing, non-zero=bitmask
    //     of applied narrowing predicates.
    void emit_with_metadata(aura::ir::IROpcode op,
                            std::uint32_t tid,
                            std::uint8_t linear_state,
                            std::uint32_t adt_variant,
                            std::uint32_t narrow_evidence,
                            std::uint32_t op0, std::uint32_t op1 = 0,
                            std::uint32_t op2 = 0, std::uint32_t op3 = 0) {
        emit_with_type(op, tid, op0, op1, op2, op3);
        if (cur_func && cur_block < cur_func->blocks.size()) {
            auto& last = cur_func->blocks[cur_block].instructions.back();
            last.linear_ownership_state = linear_state;
            last.adt_variant_id = adt_variant;
            last.narrow_evidence = narrow_evidence;
        }
    }

    void emit(aura::ir::IROpcode op, std::uint32_t op0, std::uint32_t op1 = 0,
              std::uint32_t op2 = 0, std::uint32_t op3 = 0) {
        if (!cur_func || cur_block >= cur_func->blocks.size())
            return;
        auto& blk = cur_func->blocks[cur_block];
        blk.instructions.push_back({op, {op0, op1, op2, op3}});
        // Propagate type_id from source AST node to IR instruction
        if (current_flat && current_source_id != ast::NULL_NODE &&
            current_source_id < current_flat->size()) {
            auto tid = current_flat->type_id(current_source_id);
            if (tid != 0)
                blk.instructions.back().type_id = tid;
        }
    }
    std::uint32_t alloc_block() {
        if (!cur_func)
            return 0;
        cur_func->blocks.push_back({static_cast<std::uint32_t>(cur_func->blocks.size())});
        return static_cast<std::uint32_t>(cur_func->blocks.size() - 1);
    }
};

// Free function — FlatAST path.
// Natively lowers FlatAST (SoA) to IRModule without Expr* reconstruction.
// Calls lower_flat_expr() which walks FlatAST directly.
// primitives: optional — if provided, variable references to known primitives
//             resolve to Primitive opcodes instead of ConstI64 0.
export aura::ir::IRModule lower_to_ir(ast::FlatAST& flat, ast::StringPool& pool,
                                      ast::ASTArena& arena, const Primitives* primitives = nullptr,
                                      const aura::core::TypeRegistry* type_reg = nullptr);

// Lower with cached define support.
// When cache is non-null, Call nodes whose callee is a VariableNode
// matching a cached function will inline the cached IRFunction
// (emit MakeClosure + Call) instead of lowering the callee as a variable.
// cache_hits: if non-null, receives the names of cached functions that were
// actually inlined during lowering (for dependency tracking).
export aura::ir::IRModule lower_to_ir_with_cache(
    ast::FlatAST& flat, ast::StringPool& pool, ast::ASTArena& arena,
    const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>* cache,
    std::vector<std::string>* cache_hits = nullptr, const Primitives* primitives = nullptr,
    const std::unordered_map<std::string, std::vector<aura::ir::ClosureBridgeData>>* cache_bridge =
        nullptr,
    const std::unordered_map<std::string, std::vector<std::string>>* cache_strings = nullptr,
    const std::string* self_name = nullptr, const aura::core::TypeRegistry* type_reg = nullptr);

// FlatAST → S-expression source code (reverse of parse_to_flat)
export std::string unparse_node(const ast::FlatAST& flat, const ast::StringPool& pool,
                                ast::NodeId id, int indent = 0);

// ── Result-returning variants (Issue #127) ─────────────────
//
// lower_to_ir_result is the modern, error-aware version of
// lower_to_ir. It returns LowerResult<IRModule> so callers
// can monadically chain on the error path:
//
//   auto r = lower_to_ir_result(flat, pool, arena);
//   if (!r) return std::unexpected(r.error());
//
// For now, lower_to_ir is preserved as a backward-compat
// wrapper that calls lower_to_ir_result and unwraps. New
// code should use lower_to_ir_result.
export aura::diag::LowerResult<aura::ir::IRModule> lower_to_ir_result(
    ast::FlatAST& flat, ast::StringPool& pool, ast::ASTArena& arena,
    const Primitives* primitives = nullptr,
    const aura::core::TypeRegistry* type_reg = nullptr);

export aura::diag::LowerResult<aura::ir::IRModule> lower_to_ir_with_cache_result(
    ast::FlatAST& flat, ast::StringPool& pool, ast::ASTArena& arena,
    const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>* cache,
    std::vector<std::string>* cache_hits = nullptr, const Primitives* primitives = nullptr,
    const std::unordered_map<std::string, std::vector<aura::ir::ClosureBridgeData>>* cache_bridge =
        nullptr,
    const std::unordered_map<std::string, std::vector<std::string>>* cache_strings = nullptr,
    const std::string* self_name = nullptr, const aura::core::TypeRegistry* type_reg = nullptr);

// ── Per-function lowering API (Issue #224 cycle 3) ──────────
//
// Lower a single Lambda AST node into a self-contained IRFunction.
// Unlike lower_to_ir / lower_to_ir_with_cache, this does NOT
// create a __top__ entry function — it returns the Lambda's
// function directly. Used by the per-function re-lower path
// in CompilerService::relower_define_function(), which replaces
// a single function in a cached entry's irs[] without
// re-lowering the whole bundle.
//
// The returned IRFunction has:
//   - id = 0 (caller is responsible for assigning the correct
//     func_id when inserting back into the cache)
//   - name = "__lambda__" (matches what lower_to_ir produces
//     for a top-level Define body)
//   - free_vars populated (used by the call-site re-resolution)
//   - bridge data NOT populated (the lower_to_ir pipeline sets
//     bridge data on the IRModule, not on the function)
//
// The caller (CompilerService) is responsible for:
//   - re-binding func_ids in MakeClosure instructions of the
//     new function (or any callers that referenced it)
//   - re-running per-function passes (compute_kind, constant_fold)
//   - updating the per-block dirty bitmask for the new function
//
// On failure (no Lambda found, or empty body), returns an
// empty IRFunction (blocks.empty() == true). The caller
// should check this and fall back to a full re-lower.
export aura::ir::IRFunction lower_function_at(ast::FlatAST& flat, ast::StringPool& pool,
                                              ast::ASTArena& arena, ast::NodeId lambda_node_id,
                                              const Primitives* primitives = nullptr,
                                              const std::unordered_map<
                                                  std::string,
                                                  std::vector<aura::ir::IRFunction>>* cache = nullptr,
                                              std::vector<std::string>* cache_hits = nullptr);

} // namespace aura::compiler
