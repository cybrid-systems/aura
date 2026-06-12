# IR Layer SoA/DOD Migration (Issue #167)

**Status:** Design document + Phase 1 scaffold (IRModuleV2)
**Date:** 2026-06-12
**Phase:** 1 of 5

## Problem

Aura's `FlatAST` (`src/core/ast.ixx`) already uses excellent SoA/DOD
design — `pmr::vector` columns for `tag_`, `int_val_`, `sym_id_`,
`children_`, etc. Cache locality is outstanding for AST-heavy
operations.

The **IR layer** (`src/compiler/ir.ixx`) is still AoS:

- `IRInstruction` is a single ~40-byte struct with
  `opcode`, `std::array<uint32_t, 4> operands`,
  `source_ast_node_id`, `type_id`, `shape_id`, `linear_ownership_state`,
  `adt_variant_id`, `narrow_evidence`.
- `BasicBlock` is `std::vector<IRInstruction>` (each instruction 40+ bytes).
- The interpreter (`ir_executor_impl.cpp`) and JIT bridge traverse
  these vectors instruction-by-instruction, but each touch
  pulls in 40+ bytes of data — only some of which is needed
  per access.

This is the **execution hot path** (every `eval-current`, every
JIT compile, every incremental compilation step). Cache misses
on AoS access hurt directly.

## Proposed Architecture

### SoA columns (per `IRFunction`)

```cpp
struct IRFunctionSoA {
    // Opcode stream (1 byte per instr, but stored as IROpcode enum)
    std::vector<IROpcode> opcodes_;

    // Operand columns (4 separate streams for 4-operand instructions)
    std::vector<std::uint32_t> operand0_; // result slot (when has_result_slot)
    std::vector<std::uint32_t> operand1_;
    std::vector<std::uint32_t> operand2_;
    std::vector<std::uint32_t> operand3_;

    // Metadata columns
    std::vector<std::uint32_t> source_node_ids_;
    std::vector<std::uint32_t> type_ids_;
    std::vector<std::uint32_t> shape_ids_;
    std::vector<std::uint8_t>  linear_ownership_states_;
    std::vector<std::uint32_t> adt_variant_ids_;
    std::vector<std::uint32_t> narrow_evidence_;
};
```

Total: 10 separate vectors, each accessed independently. Hot
loops that just need opcodes+operands touch 2-3 vectors
(2.5x less data than the AoS struct's 40 bytes).

### Non-owning view

```cpp
struct IRInstructionView {
    const IRFunctionSoA* func;
    std::uint32_t idx;

    IROpcode opcode() const { return func->opcodes_[idx]; }
    std::uint32_t operand(std::size_t i) const {
        switch (i) {
            case 0: return func->operand0_[idx];
            case 1: return func->operand1_[idx];
            case 2: return func->operand2_[idx];
            case 3: return func->operand3_[idx];
            default: return 0;
        }
    }
    // ... similar accessors for all metadata fields
};
```

Mirrors `NodeView` in `FlatAST` — proven pattern, well-understood.

### BasicBlock

```cpp
struct BasicBlockSoA {
    std::uint32_t block_id = 0;
    std::uint32_t start_idx = 0;   // index into IRFunctionSoA
    std::uint32_t end_idx = 0;     // exclusive
    std::vector<std::uint32_t> successors;
};
```

Blocks are now ranges into the function-level SoA. Iteration is
`for (auto i = block.start_idx; i < block.end_idx; ++i) view_at(i)`.

### IRModuleV2

```cpp
struct IRModuleV2 {
    std::vector<IRFunctionSoA> functions;
    // Plus per-function metadata (block ranges, etc.) and string pool.
    // Detailed layout TBD in Phase 2.
    std::vector<std::string> string_pool;
};
```

## Implementation Steps

### Step 1: SoA scaffold (Phase 1, this commit) ✅

- New file `src/compiler/ir_soa.ixx` with:
  - `IRFunctionSoA` (SoA columns)
  - `IRInstructionView` (non-owning view)
  - `BasicBlockSoA` (range-based)
  - `IRModuleV2` (top-level container)
  - `add_instruction` / `view_at` / iterate-by-block API
- Tests: verify basic add/get/iterate cycle preserves data
- **Zero consumer changes** — `IRModuleV2` is parallel to existing
  `IRModule`, no consumer uses it yet
- All existing tests pass (no regression)

### Step 2: LoweringSoA port (Phase 2, deferred)

- `LoweringState` learns to emit to either `IRModule` (AoS) or
  `IRModuleV2` (SoA) based on a compile-time flag
- Both paths run side-by-side; consumer picks the SoA path
  for hot code, AoS for everything else (transition period)
- Tests: parity check — same source → same execution result via
  both paths

### Step 3: ir_executor port (Phase 3, deferred)

- `ir_executor_impl.cpp`'s per-opcode handlers learn to read
  from `IRInstructionView` instead of `IRInstruction`
- Block iteration: range-based on `BasicBlockSoA`
- Performance: micro-benchmark vs AoS path

### Step 4: Pass port (Phase 4, deferred)

- ConstantFoldingWrap, TypeSpecializationWrap, EscapeAnalysis,
  LinearOwnership, TypePropagation, Inline, DCE — all consume
  IR. Each learns the SoA access pattern.
- Could provide a shim: `IRAdapter` that bridges the two
  representations if migration is too risky per-pass.

### Step 5: Remove AoS (Phase 5, deferred)

- Once all consumers migrated, delete the old `IRModule` /
  `IRInstruction` / `BasicBlock` paths
- This is the "deprecate old" step from the issue AC

## Tradeoffs

### Why a new module file (not appending to ir.ixx)

- Existing `ir.ixx` is imported by every compilation unit that
  touches IR. Appending SoA types there forces a full rebuild
  of the world on every commit to that file.
- A new module `ir_soa.ixx` is opt-in — only the SoA-aware
  consumers import it. Zero compile-time cost for existing
  code.

### Why a new IRModuleV2 type (not refactoring in-place)

- Migration risk: changing the existing `IRModule` signature
  breaks ~200+ call sites. The new type is parallel; migration
  is opt-in, per-consumer.
- Allows running both paths side-by-side during transition
  (parity tests).
- Clear deprecation target once migration is done.

### Why `std::vector` (not `std::mdspan` or `std::pmr::vector`)

- `std::mdspan` is a view, not storage. We need storage. We
  could use it for the view type, but plain `vector` is the
  simplest correct choice.
- `std::pmr::vector` is an optimization for arena-backed
  storage. The FlatAST uses it. We could adopt it here too,
  but it's a separate refactor (the `runtime_resource_` setup).
  Defer to Phase 2 or 3.

### Backward compat

- Existing `IRModule` / `IRInstruction` / `BasicBlock` stay
  unchanged. No consumer affected.
- The new `IRModuleV2` is parallel infrastructure. Phase 2
  will add a compile-time flag for the new path.

## Open Design Questions (for fresh-session pickup)

1. **Block storage:** should `BasicBlockSoA::successors` store
   `uint32_t` block indices (current plan) or
   `IRBlockId`-style strong types? Recommendation: strong
   types for safety, defer if it adds friction.

2. **String pool ownership:** `IRModuleV2::string_pool` is
   currently a copy of `IRModule::string_pool`. Could be
   `shared_ptr` if we want to share. Defer to Phase 2.

3. **Function-level vs module-level columns:** the SoA
   columns are per-function today. If we want
   module-level iteration, we'd need parallel "all opcodes"
   vectors at the module level. Defer to Phase 3.

4. **JIT bridge impact:** the JIT code generation reads
   `IRInstruction` fields directly. Migrating to
   `IRInstructionView` is mechanical but needs care
   for the inlined-assembly path. Defer to Phase 3.

5. **Migration of the existing `IRModule` consumers:**
   the in-place refactor approach (rename + change types) is
   the long-term answer. But the "parallel IRModuleV2 +
   shim" approach is safer. Decision: shim first, in-place
   refactor in Phase 5.

## Commits This Phase

- TBD: this design doc + IRModuleV2 scaffold + tests

## Phase 2-5 (deferred to fresh session)

- Phase 2: LoweringSoA port
- Phase 3: ir_executor port + micro-benchmarks
- Phase 4: Pass port (constant folding, type spec, etc.)
- Phase 5: Remove old AoS paths

## Why Phase 1 Only Tonight

At 38+ hours continuous, the minimum safe scope is:
1. Design doc (the deliverable for deferred work — fresh
   session picks up from here)
2. IRModuleV2 scaffold (pure infrastructure, zero consumer
   impact)
3. Basic tests (verify the API works)

The actual consumer migration (Steps 2-5) is 18-37 hours
focused work. Per-consumer migration needs design review
and careful testing that doesn't fit in the remaining
session energy.
