# Sound Gradual Typing — Implementation Plan

> Issue #40 — 实现完整的 Sound Gradual Typing coercion + 运行时检查 + blame 追踪

## 当前状态

```
                   类型检查阶段                    lowering 阶段                运行时
┌─────────────────────────────────┐   ┌────────────────────────┐   ┌─────────────┐
│ type_checker_impl.cpp           │   │ lowering_impl.cpp      │   │ IR Executor │
│                                 │   │                        │   │             │
│ ✓ synthesize_flat → 类型推导    │   │ ✓ TypeAnnotation→CastOp│   │ ✓ CastOp解释 │
│ ✓ is_coercible → 插CoercionNode │──→│ ✓ CoercionNode→CastOp  │──→│ ✓ 运行时检查  │
│ ✓ set_loc + blame_node 传递     │   │ ✓ blame_node 存type_id │   │ ✓ report_    │
│                                 │   │ ✓ optimize_type_info   │   │   blame+node │
└─────────────────────────────────┘   └────────────────────────┘   └─────────────┘
```

### 已完成

**Phase 1 (✅): CoercionNode 自动插入**
- `check_flat_call`: 参数类型不匹配时插 CoercionNode
- `check_flat_call`: 返回类型不匹配时插 CoercionNode（wrap 整个 call）
- `check_flat`: 通用表达式类型不匹配时插 CoercionNode
- `synthesize_flat(Coercion)`: 返回目标类型而非 inner 类型

**Phase 2 (✅): 源位置 + CastOp blame 改进**
- CoercionNode 插入时 `set_loc()` 复制源表达式 line/col
- CoercionNode lowering 用 `emit_with_type` 传 blame_node 到 CastOp
- `report_blame()` 输出包含 `(node N)`，Agent 可定位到具体 AST 节点

**Phase 3 (✅): 测试**
- `tests/gradual_typing.aura`: 9 个测试

```
                   类型检查阶段                    lowering 阶段                运行时
┌─────────────────────────────────┐   ┌────────────────────────┐   ┌─────────────┐
│ type_checker_impl.cpp           │   │ lowering_impl.cpp      │   │ IR Executor │
│                                 │   │                        │   │             │
│ synthesize_flat → 类型推导      │   │ TypeAnnotation → CastOp│   │ CastOp 解释  │
│ is_coercible → emit Note（只报  │──→│ CoercionNode → CastOp  │──→│ 含运行时检查  │
│    告不插入节点）               │   │ optimize_type_info 消  │   │ report_blame│
│                                 │   │ 冗余 CastOp            │   │             │
└─────────────────────────────────┘   └────────────────────────┘   └─────────────┘
```

### 已有能力
- **CoercionNode** (AST tag 0x10) — 已有完整定义，`fixed_children=1`, `has_string=true`，含 `type_tag` + `type_id`
- **CastOp** (IROpcode) — 已有 lowering 和运行时解释器，支持 Int/String/Bool/Float ⟷ Dynamic 互转
- **is_coercible(from, to)** — 已有类型间可转换性判断
- **optimize_type_info** — 已有冗余 CastOp 消除 pass

### 缺失能力
1. **CoercionNode 不插入** — `is_coercible` 只在 type_check 时 emit Note，不修改 AST
2. **Blame 追踪不精确** — 当前只传 line/col，不传 NodeId，定位不到具体 AST 子树
3. **缺失测试** — 无 gradual typing 集成测试

## 设计方案

### Phase 1: CoercionNode 自动插入（类型检查阶段）

在 `type_checker_impl.cpp` 的 check 函数中，当 `consistent_unify` 失败但 `is_coercible` 成功时，**在 AST 中插入 CoercionNode**：

```cpp
// 修改 check_flat_call / check_flat_lambda / check_flat 等
if (!cs_.consistent_unify(arg_type, ft.args[i])) {
    if (is_coercible(arg_type, ft.args[i])) {
        // NEW: 插入 CoercionNode 到 AST
        auto coercion_id = flat.add_coercion(arg_id, type_tag, ft.args[i].index);
        flat.set_type(coercion_id, ft.args[i].index);
        // 用 coercion_id 替换 arg_id 的引用
    }
}
```

**涉及函数：**
- `check_flat_call` — 函数调用参数类型不符时插入 CoercionNode
- `synthesize_flat_if` — if 分支类型合并时
- `synthesize_flat_let` — let 绑定值类型不匹配时
- `check_flat` / `synthesize_flat` 递归入口

### Phase 2: CastOp 完整性检查

当前 CastOp 运行时只做**值级别**转换（Int↔String 等）。需要确保：

- **Dynamic → Static**：当无类型代码（标记为 Dynamic/Any）进入静态类型区域时，生成运行时类型标签检查
- **Static → Dynamic**：直接透传，不需要检查（安全向上转型）

```
动态 ──→ 静态: 插入 CastOp，运行时检查类型标签是否匹配
静态 ──→ 动态: 直接透传，无需检查
静态 ──→ 静态 (不同类型): 插入 CastOp，运行时转换
```

### Phase 3: Blame 追踪

改进 `report_blame` 使其能关联到原始 AST NodeId：

```cpp
// CastOp 增加一个字段：blame_node (NodeId)
struct CastOp_extra {
    std::uint32_t blame_loc;    // 已有，line<<16|col
    ast::NodeId blame_node;     // NEW: 源 AST 节点 ID
};
```

在 lowering 时，从 CoercionNode 的 `type_id` 和 `int_val`(type_tag) 读取，同时记录源 NodeId。
在 IR executor 报错时，输出 NodeId 以便 Agent 通过 `(ast:summary)` 或 `(query:parent)` 定位。

### Phase 4: Dynamic 类型 + Any 顶类型

- 当函数参数声明为 `Any`（或未标注）时，允许传入任何类型，不做运行时检查
- 当返回值声明为 `Any` 时，调用者获得 Dynamic 类型
- `Any` 和 `Dynamic` 在 TypeRegistry 中使用同一 ID（`DYNAMIC = 0`）

## 当前状态

```
                   类型检查阶段                    lowering 阶段                运行时
┌─────────────────────────────────┐   ┌────────────────────────┐   ┌─────────────┐
│ type_checker_impl.cpp           │   │ lowering_impl.cpp      │   │ IR Executor │
│                                 │   │                        │   │             │
│ ✓ synthesize_flat → 类型推导    │   │ ✓ TypeAnnotation→CastOp│   │ ✓ CastOp解释 │
│ ✓ is_coercible → 插CoercionNode │──→│ ✓ CoercionNode→CastOp  │──→│ ✓ 运行时检查  │
│ ✓ Dynamic→Static 边界检查       │   │ ✓ Dynamic→Zero CastOp  │   │ report_blame │
│ ✓ Any 类型签名支持              │   │ ✓ optimize_type_info   │   │ + NodeId     │
└─────────────────────────────────┘   └────────────────────────┘   └─────────────┘
```

### 已完成

**Phase 1 (✅): CoercionNode 自动插入**
- `check_flat_call`: 参数/返回类型不匹配时插 CoercionNode
- `check_flat`: 通用表达式类型不匹配时插 CoercionNode
- `synthesize_flat(Coercion)`: 返回目标类型而非 inner 类型

**Phase 2 (✅): 源位置 + CastOp blame 改进**
- CoercionNode 插入时 `set_loc()` 复制源表达式 line/col
- CoercionNode lowering 用 `emit_with_type` 传 blame_node 到 CastOp
- `report_blame()` 输出包含 `(node N)`

**Phase 3 (✅): 测试**
- `tests/gradual_typing.aura`: 12 个测试

**Phase 4 (✅): Dynamic 类型 + Any 顶类型**
- Dynamic → Static 边界：consistent_unify 成功后仍插 CoercionNode 做运行时 check
- TypeAnnotation lowering: dynamic(0) → static 时也发 CastOp
- `Any` 类型签名作为参数时正确通行（Any = DYNAMIC，consistent with everything）
- 3 个 Any 类型专项测试

## 实现路径

```cpp
// 在 check_flat_call 中，参数类型不匹配时
if (is_coercible(arg_type, ft.args[i])) {
    auto type_tag = type_tag_for_coercion(ft.args[i], reg_);
    auto coercion_id = flat.add_coercion(arg_id, type_tag, ft.args[i].index);
    // 把父节点的 child 引用改为 coercion_id
    // 需要 set_child 能力
}
```

### Step 2: 修改 synthesize_flat_if / synthesize_flat_let

if 表达式两个分支类型不同但可 coercion 时插入 CoercionNode。
let 绑定的值与类型标注不匹配时插入。

### Step 3: 改进 CastOp blame 传递

- `CoercionNode` 用 `line_`/`col_` 字段存储源位置
- lowering 时从 CoercionNode 读取 `NodeId` 传给 `CastOp`
- IR executor 报错时输出 `blame_node`

### Step 4: 添加测试

```aura
;; 测试 1: Int → String coercion
(string-append "value: " 42)  ; 42 应自动转为 "42"

;; 测试 2: String → Int coercion
(+ 1 "2")                      ; "2" 应自动转为 2

;; 测试 3: Dynamic → Static blame
(define (f [x : Int]) x)
(f "hello")                    ; 运行时报错，blame 指向 (f "hello")
```

## 风险与注意事项

1. **性能开销**: CastOp 运行时检查有 <10% 的开销预算，optimize_type_info 能消除冗余检查
2. **CoercionNode 插入时机**: 必须在类型检查阶段而非 parse 阶段，因为类型信息在 type-check 时才知道
3. **副作用**: 插入 CoercionNode 会修改 AST，需要确保 dirty 标记正确传播
4. **JIT 路径**: CastOp 的 LLVM JIT 路径也需要支持运行时检查（当前仅有 IR interpreter 路径）
