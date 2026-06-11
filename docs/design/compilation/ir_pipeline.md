# IR Pipeline Design

**Status**: Implemented (2026-06)
**Design Author**: Anqi Yu + Ani

---

## 0. Implementation Status (2026-06-11, Issue #156)

**重要**：本文档的 **IR 管线已全部实装并成为 `eval()` 默认路径**（tree-walker 仅作 fallback）。模块系统、EDSL 原语、特殊形式等有意保留 fallback。准确分两层：

### C++ Core Layer (`src/compiler/ir.ixx` / `lowering.ixx` / `lowering_impl.cpp` / `ir_executor.ixx` / `ir_executor_impl.cpp` / `pass_manager.ixx` / `service.ixx` / `evaluator.ixx` / `evaluator_impl.cpp`)

| 组件 | 实装 | 备注 |
|------|------|------|
| `IROpcode` 扩展 (`Primitive` / `ConstBool` / `ConstVoid`) | ✓ | `ir.ixx` |
| Primitives-aware lowering (`LoweringState.primitives_`) | ✓ | `lowering_impl.cpp` Variable handler — 100+ 原语全部可走 IR |
| Bool semantics (`make_bool` for Eq/Lt/And/Or/Not) | ✓ | `ir_executor_impl.cpp` + `pass_manager.ixx`（constant folding 用 `replace_bool`）|
| Chained comparisons (pairwise AND) | ✓ | `lowering_impl.cpp`；`(= 5 5 5)` 不再错成 `(= (= 5 5) 5)` |
| Closure bridge (`IRClosure` ↔ `Closure`) | ✓ | 3-part：lowering 存 bridge data → MakeClosure 复制 → `apply_closure` callback |
| Pair/Quote lowering (cons chain) | ✓ | `lowering_impl.cpp`；`'(1 2 3)` 不再触发 fallback |
| Bool literal (`#t`/`#f` → `ConstBool` via `SyntaxMarker::BoolLiteral`) | ✓ | `lowering_impl.cpp`；常量化保留 bool 类型 |
| `DeadCoercionEliminationPass` | ✓ | 消除冗余 CastOp（T2e 实装）|
| `eval()` 统一入口 + `needs_tree_walker_fallback()` | ✓ | `service.ixx`；scan-based fallback（避免 silent 错结果）|
| `define` IR cache (`ir_cache_` + 依赖追踪) | ✓ | `service.ixx`；增量 recompilation 兼容 |
| Module system (`import`/`use`/`require`) | ✗ (intentional fallback) | env-binding side effects IR can't replicate |
| EDSL (`query:*`/`mutate:*`/`set-code`) | ✗ (intentional fallback) | mutation log + persistent AST |
| Special forms (`try`/`catch`/`when`/`unless`/`cond`/`export`) | ✗ (intentional fallback) | Not IR primitives yet |
| Native Pair IR ops (`MakePair`/`Car`/`Cdr`) | 🟡 (Future Step 3) | 当前走 `cons` call chain；可后续替换为 native ops |
| Per-module dirty skip optimization | 🟡 (Future Step 4) | 当前 per-IRFunction dirty 粒度 |
| IR-level module loading (eliminate module fallback) | 🔴 (Future Step 5) | `import` 仍走 fallback |
| `try`/`catch` IR 支持 | 🔴 (Future Step 6) | 异常 opcodes 未实装 |
| LLVM JIT 后端 | 🟡 (separate doc) | `compilation/jit.md` — 实装于 jit.md 的 Phase 1-5 |
| AOT 编译 | 🔴 (Future Step 8) | 设计稿未启动 |
| Self-hosting bootstrap | 🔴 (Future Step 9) | 长期 |

### Aura Layer (无 EDSL 包装)

`ir_pipeline` 是 **C++ 内部优化**，无 Aura 表面调用。原语 `eval` 内部自动选 IR 或 tree-walker。

### 已实现 vs 计划

- ✅ **已实装**：上述 ✓ 项；IR 是 `eval()` 默认路径，tree-walker 作 fallback
- 🟡 **未来 1-2 个 step**：native pair ops / per-module dirty skip / module loading
- 🔴 **长期**：self-hosting bootstrap

**AI Agent 读者请注意**：IR 管线在 `eval()` 内部自动选择（tree-walker 仅 fallback）。Agent 写代码时无需关心 IR vs 树遍历差异。完整 EDSL 能力仍通过原语或 `--serve` 协议使用；详见 `design/core/query_edsl.md` 和 `mutate_api.md` 的 §0 状态表。

---


Aura has two execution engines:
1. **Tree-walker evaluator** (`eval_flat`) — interprets FlatAST directly; handles all language features
2. **IR pipeline** (`lower_to_ir` → passes → `IRInterpreter`) — lowers FlatAST to IR bytecode, runs optimization passes, interprets IR

Goal: make the IR pipeline the **default** execution path, with the tree-walker as a **fallback** for constructs the IR can't handle.

## Two-Phase Architecture (Before)

```
eval() → parse → macro_expand → eval_flat (tree-walker)     ← default
eval_ir() → parse → [MacroDef? → fallback] → lower_to_ir → passes → IRInterpreter  ← explicit alternate
```

`eval_ir()` already existed but was never the default. It handled:
- `define` specially: cache IR in `ir_cache_` + eval via tree-walker for env persistence
- Normal expressions: lower → passes → execute
- Macros: detect `MacroDef` nodes → fallback to tree-walker

## Unified Entry Point (After)

```
eval() → parse → macro_expand → needs_fallback?
  ├── yes → eval_flat (tree-walker)
  └── no  → lower_to_ir_with_cache → passes → IRInterpreter → result
```

### Fallback Detection

`needs_tree_walker_fallback()` scans the FlatAST for constructs the IR can't handle:

| Condition | Reason |
|-----------|--------|
| `MacroDef` node | Macros need runtime expansion |
| `Quote` with Pair/Call child | Now handled (2026-05-17) — see Pair/Quote lowering |
| `Lambda` node | Now handled via closure bridge (2026-05-17) |
| EDSL call callee (`query:*`, `mutate:*`, `set-code`, etc.) | Needs evaluator internal state (mutation log, persistent AST) |
| Special form callee (`when`, `unless`, `try`, `catch`, `raise`, `export`, `cond`) | Not implemented as IR primitives |
| Module system callee (`import`, `use`, `require`) | `import` has env-binding side effects IR can't replicate |
| Non-primitive call callee (not in primitives table, not in ir_cache) | May come from runtime `import` |
| Bare root-level variable not in primitives/cache | May be from runtime import (e.g., `pi`) |

### Define Handling

Top-level `(define ...)` in `eval()`:
1. Extract define name
2. Lower to IR with cache awareness
3. Cache all non-entry functions in `ir_cache_[name]`
4. Track dependencies for incremental recompilation
5. Evaluate via tree-walker for persistent runtime bindings
6. Return void (define produces no visible output)

## Changes Made

### 1. IROpcode Additions (`ir.ixx`)

| Opcode | Purpose | Operands |
|--------|---------|----------|
| `Primitive` | Load a primitive function value by slot index | result_slot, prim_slot |
| `ConstBool` | Load boolean constant (`#t`/`#f`) | result_slot, value(0/1) |
| `ConstVoid` | Load void (empty list `()`) | result_slot |

### 2. Primitives-Aware Lowering (`lowering_impl.cpp`)

The `LoweringState` now carries a `const Primitives*` pointer. When the Variable handler encounters a name not in scope/free_vars/cache:

```cpp
// Before: ConstI64 0 (wrong for primitives like cons, car, +, etc.)
// After: check Primitives table → emit Primitive opcode
```

This allows ALL primitives (100+) to be available in IR code, not just the subset in `prim_call_map`.

### 3. Bool Semantics (`ir_executor_impl.cpp`, `pass_manager.ixx`)

Comparisons and logic operations now return `make_bool()` instead of `make_int(0/1)`:

| Opcode | Before | After |
|--------|--------|-------|
| Eq, Lt, Gt, Le, Ge | `make_int(result ? 1 : 0)` | `make_bool(result)` |
| And, Or, Not | `make_int(... ? 1 : 0)` | `make_bool(...)` |

The constant folding pass uses `replace_bool()` instead of `replace()` for comparison/logic opcodes, preserving the bool type through folding: `(= 1 1)` → `ConstBool(#t)` not `ConstI64(1)`.

### 4. Chained Comparisons (`lowering_impl.cpp`)

Chained comparisons like `(= 5 5 5)` and `(<= 1 3 2)` use **pairwise AND** instead of boolean chaining:

```cpp
// (= a b c) → (and (= a b) (= b c))
// Not: (= (= a b) c)  ← wrong, compares bool to value
```

### 5. Closure Bridge (`evaluator.ixx`, `ir_executor.ixx`, `service.ixx`)

**Problem**: IR closures (`IRClosure`) and tree-walker closures (`Closure`) are incompatible types stored in separate maps. Primitives like `map`, `filter`, `foldl` call closures through the tree-walker's `closures_` map, so IR closures are invisible to them.

**Solution**: Three-part bridge:

```
IRFunction ──closure_bridge──▶ ClosureBridgeData{flat*, pool*, body_id}
                                ↓
IRClosure ──copied at MakeClosure──▶ flat*, pool*, body_id, params
                                      ↓
Evaluator ──closure_bridge_ callback──▶ looks up IR closure → builds Env → eval_flat
```

**Data flow**:
1. **Lowering**: Lambda handler stores `(flat*, pool*, body_id)` in `IRModule::closure_bridge[func_id]`
2. **MakeClosure**: Copies bridge data into `IRClosure.flat`, `.pool`, `.body_id`, `.params`
3. **apply_closure()**: New method on `Evaluator` — first checks `closures_` (tree-walker), then calls `closure_bridge_` (IR)
4. **Bridge callback**: Set in `service.ixx` before each `IRInterpreter::execute()` — looks up IR closure by id, extracts env/params, builds tree-walker `Env`, calls `eval_flat`
5. **Cleanup**: Bridge cleared after execution to avoid dangling references

**Updated primitives**: `map`, `filter`, `foldl` now use `apply_closure()` instead of direct `closures_.find()`.

### 6. Pair/Quote Lowering (`lowering_impl.cpp`)

`(quote data)` where `data` is a list or pair is lowered as a `(cons ...)` chain instead of falling back:

```scheme
'(1 2 3)    →  cons(1, cons(2, cons(3, (void))))
'(a . b)    →  cons(a, b)
'((1 2) 3)  →  cons(cons(1, cons(2, void)), cons(3, void))
```

The `lower_q` recursive lambda handles:
- `LiteralInt` → inline
- `LiteralFloat` / `LiteralString` → lower normally
- `Pair` → `(cons lower_q(car) lower_q(cdr))`
- `Call` → `(cons lower_q(child[0]) cons(lower_q(child[1]) ...))`

Uses `Primitive` opcode for `cons` and `Call` opcode to invoke it at runtime.

### 7. Bool Literal Fix (`lowering_impl.cpp`)

`#t`/`#f` are parsed as `LiteralInt(1/0)` with `SyntaxMarker::BoolLiteral`. The lowering checks the marker and emits `ConstBool` instead of `ConstI64`:

```cpp
if (marker == SyntaxMarker::BoolLiteral)
    emit(ConstBool, slot, value);
else
    emit(ConstI64, slot, value);
```

## Remaining Fallbacks

These are intentional — they need evaluator state that the IR doesn't provide:

- **Module system** (`import`/`use`/`require`) — env-binding side effects
- **EDSL** (`query:*`/`mutate:*`/`set-code`) — mutation log, persistent AST
- **Special forms** (`try`/`catch`/`when`/`unless`/`cond`/`export`) — not IR primitives

## Future Steps

| Step | Description | Status |
|------|-------------|--------|
| 3 | Native Pair IR ops (MakePair, Car, Cdr) | 🟡 |
| 4 | Per-module dirty skip optimization | 🟡 |
| 5 | IR-level module loading | 🔴 |
| 6 | `try`/`catch` IR support | 🔴 |
| 7 | LLVM JIT backend | 已移至 `compilation/jit.md`（Phase 1-5 实装） |
| 8 | AOT compilation | 🔴 |
| 9 | Self-hosting bootstrap | 🔴 |

## Code Locations

| File | Purpose |
|------|---------|
| `src/compiler/ir.ixx` | IROpcode enum, PrimId, IRModule, IRFunction, BasicBlock |
| `src/compiler/lowering.ixx` | LoweringState, lower_to_ir declarations |
| `src/compiler/lowering_impl.cpp` | FlatAST → IR bytecode lowering |
| `src/compiler/ir_executor.ixx` | IRInterpreter class, IRClosure |
| `src/compiler/ir_executor_impl.cpp` | IR bytecode interpreter |
| `src/compiler/pass_manager.ixx` | Passes: compute-kind, arity check, constant folding |
| `src/compiler/service.ixx` | CompilerService: eval() unified entry, fallback detection |
| `src/compiler/evaluator.ixx` | Evaluator: apply_closure(), closure_bridge_ |
| `src/compiler/evaluator_impl.cpp` | apply_closure implementation, bridge primitives |

## Key Design Decisions

1. **Scan-based fallback** over try-IR-fallback: avoids silent wrong results from IR unsupported constructs
2. **Lambda fallback → closure bridge**: higher complexity but eliminates the biggest remaining IR coverage gap
3. **Bridge via std::function callback**: cleaner than shared closure ID space or Evaluator-IRInterpreter coupling
4. **cons chain for quoted lists** over Pair IR ops: simpler, faster to implement, easy to swap for native ops later
5. **make_bool for comparisons** over int: matches tree-walker semantics exactly, eliminates format issues
6. **ConstBool opcode** over CastOp: cleaner than adding CastOp everywhere, constant folding handles correctly
