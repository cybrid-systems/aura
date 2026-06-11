# Type Information Flow into IR (Issue #27)

**Status**: Design
**Design Author**: Ani

## Problem

Issue #27: `infer_flat()` returns `TypeId` per AST node, but this information does not reliably propagate into the IR pipeline. `IRInstruction` has a `type_id` field (Phase 2, commit `9e10331`), but the auto-propagation mechanism via `current_source_id` is broken for compound expressions, and many instructions always get `type_id = 0`.

### Root Cause: `current_source_id` overwrite bug

The current propagation mechanism in `LoweringState::emit()` (lowering.ixx) reads `current_flat->type_id(current_source_id)` to annotate each IR instruction:

```
lower_flat_expr(id):               // current_source_id = id (e.g., Call: (+ 1 2))
  ├── lower_flat_expr(child1):     // current_source_id = child1 (LiteralInt 1)
  │   └── emit(ConstI64)           // ← type_id = INT ✓ (correct for child1)
  ├── lower_flat_expr(child2):     // current_source_id = child2 (LiteralInt 2)
  │   └── emit(ConstI64)           // ← type_id = INT ✓ (correct for child2)
  └── emit(OpAdd)                  // ← current_source_id = child2 ✗
                                    //   result gets type_id of literal 2,
                                    //   not the Call node's inferred type
```

**Result**: Only leaf instructions (literals, variables) get correct type_id. All compound expression results (`OpAdd`, `OpCall`, `MakePair`, etc.) get a stale/wrong type_id — often from the last processed child, or 0 when type checking didn't annotate that child.

### Broader Gaps

1. **No fallback for untyped paths**: When the IR pipeline runs without prior type checking (interactive REPL, quick eval fallback), `flat.type_id()` returns 0 for everything.
2. **No typed IR pass**: `optimize_type_info()` exists but is unreachable because type_ids are never populated. No `TypedIRPass` exists.
3. **JIT/AOT blind**: Codegen has no type information for better LLVM IR generation.

## Design

### Phase A: Fix Propagation (this issue)

**Strategy**: RAII scope guard + explicit override for `emit()`.

#### 1. Add `SourceScope` RAII guard

```cpp
// In LoweringState
struct SourceScope {
    LoweringState& state;
    ast::NodeId saved;
    SourceScope(LoweringState& s, ast::NodeId id)
        : state(s), saved(s.current_source_id) {
        state.current_source_id = id;
    }
    ~SourceScope() { state.current_source_id = saved; }
};
```

Place at the top of every `lower_flat_expr` entry:

```cpp
SourceScope scope(state, id);
```

This guarantees:
- `current_source_id` = parent node when parent emits its result instruction
- Children get proper scope for their own emits
- No stale overwrite after child processing completes

#### 2. Add explicit `emit_with_type()` overload

For cases where the result type differs from the source node (e.g., a CastOp's result type is the target type, not the source expression's type):

```cpp
void emit_with_type(aura::ir::IROpcode op, uint32_t tid,
                    std::uint32_t op0, std::uint32_t op1 = 0,
                    std::uint32_t op2 = 0, std::uint32_t op3 = 0) {
    emit(op, op0, op1, op2, op3);
    if (cur_func && tid != 0)
        cur_func->blocks[cur_block].instructions.back().type_id = tid;
}
```

Used for:
- `CastOp` emission (`lowering_impl.cpp:676`) — target type known from context
- `Call` result type — from callee return type (if known)

#### 3. No change to `IRInstruction` / `optimize_type_info()`

Both already exist and are correct. The fix is solely in lowering propagation.

### Phase B: JIT/AOT Usage (future)

Once type_id is reliably populated:
- `aura_jit.cpp`: skip tagging/boxing for known primitive types (i64, f64)
- AOT: emit native LLVM types instead of tagged i64
- Requires ownership consistency (DropOp/MoveOp must match between typed/untyped paths)

### Phase C: TypedIRPass (future, Level 3)

A full pass that:
- Propagates types across basic block boundaries (phi node type inference)
- Validates type consistency
- Eliminates redundant CastOps reliably
- Feeds type info to LLVM codegen

## Implementation Plan

1. Add `SourceScope` RAII guard to `LoweringState` in `lowering.ixx`
2. Add `emit_with_type()` overload
3. Fix `lower_flat_expr`: add `SourceScope scope(state, id)` at key entry points
4. Fix `CastOp` emission to use `emit_with_type()` with correct target type
5. Add regression tests:
   - Verify type_id on result instructions of `(+ 1 2)` style compound exprs
   - Verify CastOp has correct target type_id
   - Run existing fuzz and AOT tests for zero regression

## References

- Issue #27
- Phase 2 commit `9e10331`
- `docs/design/aura_typesystem.md` §5.2
- `docs/design/ir_pipeline_design.md`
- Roadmap Level 3
