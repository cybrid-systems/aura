# Primitive Inlining — 消除运行时查表开销

## 现状

`(evo-set "k" "v")` 的调用链路：

```
Parser → FlatAST → TCO loop
  → lookup_primitive("evo-set")      ← O(1) 但 hash 查表
  → 取 PrimFn (std::function)
  → 调用 (args) → 返回值
  → 输出格式化为 EvalValue
```

`lookup_primitive` 在 `Prerequisites::table_` (unordered_map) 中查找。每次调用都走 `std::function` 的 indirect call。对于短操作（hash set 本身 ~50ns），查表 + 间接调用占 50%+ 时间。

## 方案

### 阶段 1：IR PrimCall 直通 slot index

IR lowering 已支持 `IROpcode::PrimCall`。当前是 `primitives_.lookup(name) → slot`。改为在 lowering 阶段确定 slot index，emit 时直接用 slot 编号：

```
;; 当前 IR:
PrimCall prim_id="evo-set" packed_args=(0,1,2) result_slot=3

;; 阶段1 IR:
PrimCall prim_slot=42 packed_args=(0,1,2) result_slot=3
```

省去运行时 string lookup。

### 阶段 2：InlinePrimitive 指令

对纯函数 primitives（`+`, `-`, `hash-set!`, `hash-ref` 等）添加 `IROpcode::InlinePrim`：

```
IROpcode::InlinePrim {
    opcode: InlinePrim,
    operands: [
        result_slot,       // 输出 slot
        car_slot,          // 第一个 arg slot
        cdr_slot,          // 第二个 arg slot（若无则填 NULL_NODE）
        prim_op,           // enum: HASH_SET, HASH_REF, HASH_REMOVE, STR_APPEND, ...
    ]
}
```

IR interpreter 中 direct dispatch：

```cpp
case IROpcode::InlinePrim: {
    auto& a = locals[ops[1]];
    auto& b = locals[ops[2]];
    auto op = static_cast<InlinePrimOp>(ops[3]);
    switch (op) {
        case InlinePrimOp::HASH_SET: {
            // hash-set! 直接内联：不经过 std::function dispatch
            auto* ht = is_hash(a) ? &as_hash(a) : nullptr;
            if (ht) {
                hash_set(ht, a, b);
                locals[ops[0]] = make_void();
            }
            break;
        }
        // ...
    }
    break;
}
```

### 阶段 3：JIT/AOT 完全内联

LLVM JIT/AOT 路径中，对 `InlinePrim` 直接 emit 对应的 LLVM IR 函数调用，跳过所有 Aura 运行时包装：

```
; LLVM IR
%ht = load %hash*, %hash** @evo_store
call void @hash_set(%hash* %ht, %EvalValue %key_val, %EvalValue %val_val)
%result = alloca %EvalValue
store %EvalValue {i64 0, i64 0}, %EvalValue* %result  ; make_void()
```

## 改动文件

| 文件 | 改动 |
|------|------|
| `src/compiler/ir.ixx` | 添加 `InlinePrim` opcode + `InlinePrimOp` enum |
| `src/compiler/lowering_impl.cpp` | `PrimCall` -> `InlinePrim` 下沉（基于 primitive 的 slot 和 arg 类型） |
| `src/compiler/ir_executor_impl.cpp` | `case InlinePrim:` 直接调用 hash/str/vec C 函数 |
| `src/compiler/aura_jit.cpp` | LLVM IR 生成：`InlinePrim` -> 直接调 C 函数 |

## 收益

| 阶段 | 加速 | 说明 |
|------|------|------|
| 1: slot index | ~10-15% | 省 string lookup |
| 2: InlinePrim | ~30-50% | 省 std::function dispatch |
| 3: LLVM inline | ~2-3x | 完全内联到 native code |

## 风险

- IR executor 代码膨胀（每个 InlinePrimOp 要一个 case）
- JIT/AOT 路径需要为每个 hash/str/vec 操作暴露 C linkage
- 与现有 PrimCall 路径的互操作（一些 primitive 需要闭包/环境）
