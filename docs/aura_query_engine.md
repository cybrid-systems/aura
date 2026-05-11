# M2: AuraQuery 引擎设计方案

**版本**：v0.1（草案）
**依赖**：M1 IR 管线 ✅（FlatAST + AuraIR + PassManager）
**状态**：设计阶段

---

## 1. M2 定位

M1 完成了"把代码变成可执行的数据"——解析 → AST → IR → 执行。
M2 要做的：**让 AI 能在这个数据上查询、分析、变换**。

```
M1:  Code  →  ASTFlat  →  AuraIR  →  Execute
     (编译管线, 单向)

M2:  Query  →  Index   →  Match   →  Transform
     (查询变换引擎, 双向)
```

M2 不是单独的编译器，而是**架在 M1 数据之上的查询/变换层**。AI Agent 通过 M2 在 AST/IR 上进行：
- 语义查询（"找到所有递归调用"）
- 模式匹配（"找到 `(* x 2)` 的所有出现"）
- 自动修复（"把 `(+ x x)` 替换为 `(* x 2)`"）
- 热更新（"替换函数 `fact` 的实现"）

---

## 2. 核心架构

```
AuraQueryEngine
│
├── Index Layer         ← 在 FlatAST / AuraIR 上建索引
│   ├── AST Index       节点类型 / 模式 / 子树
│   ├── IR Index        IR 指令 / 基本块 / 函数
│   ├── Def-Use Index   定义-使用链
│   └── String Index    符号（SymId）倒排
│
├── Query Layer
│   ├── Query Parser    解析查询表达式
│   ├── Matcher         模式匹配引擎
│   └── Result Set      节点集合 / 统计
│
├── Mutation Layer
│   ├── Patch Generator 查询结果 → 补丁
│   ├── Patch Applier   补丁 → FlatAST 修改
│   └── Hot Swap        函数级 / 子树级替换
│
└── AI Interface
    ├── Query API       给 Agent 的直接接口
    ├── Fix Pipeline    报错 → 查询 → 修复 → 验证
    └── Event Hook      编译结果通知
```

---

## 3. 索引层 (Index Layer)

### 3.1 AST Index

建立在 FlatAST（SoA 结构）上，**零额外存储**查询：

```cpp
// 已有 FlatAST: 9 个 pmr::vector
// 查询直接在 SoA 上迭代
struct ASTIndex {
    const FlatAST& ast;

    // 按标签过滤
    auto by_tag(NodeTag t) const {
        return std::views::iota(0u, ast.size())
             | std::views::filter([&](NodeId id) {
                   return ast.get(id).tag == t;
               });
    }

    // 按子节点关系过滤
    auto by_child(NodeId parent, uint32_t idx) const {
        auto v = ast.get(parent);
        if (idx < v.children.size())
            return v.children[idx];
        return NULL_NODE;
    }
};
```

关键设计：**不复制。** FlatAST 已经是 SoA，迭代就是索引。

### 3.2 IR Index

建立在 IRModule 上，类似原理：

```cpp
struct IRIndex {
    const IRModule& mod;

    // 按 opcode 过滤指令
    auto by_opcode(IROpcode op) const {
        return mod.functions
             | std::views::transform(&IRFunction::blocks)
             | std::views::join
             | std::views::transform(&BasicBlock::instructions)
             | std::views::join
             | std::views::filter([op](const IRInstruction& i) {
                   return i.opcode == op;
               });
    }
};
```

### 3.3 String Index (SymId 倒排)

基于已实现的 StringPool：

```cpp
// 通过 SymId 查找所有引用该符号的节点
struct SymRefIndex {
    // 构建时扫描 FlatAST 一次
    // 输出: SymId → vector<NodeId>
    std::pmr::unordered_multimap<SymId, NodeId> refs_;

    // 查询："所有引用 'fact' 的节点"
    auto refs_of(SymId s) const {
        return refs_.equal_range(s);
    }
};
```

---

## 4. 查询层 (Query Layer)

### 4.1 AuraQuery DSL

查询语言设计目标：**Lisp S-表达式，AI 原生友好。**

```
;; 查询所有 Call 节点
(query (node-type Call))

;; 查询所有调用特定函数的 Call
(query (node-type Call)
       (child 0 (node-type Variable)
               (= name "fact")))

;; 查询所有递归调用（函数体内部调用自己）
(query (node-type Lambda)
       (exists (child (node-type Call)
                      (child 0 (node-type Variable)
                              (= name :parent-name)))))

;; 查询未使用的定义
(query (node-type Define)
       (= (ref-count :node) 0))

;; 组合查询
(query (and (node-type Call)
            (> (child-count) 1)
            (child 0 (node-type Variable))))
```

### 4.2 查询原语

| 原语 | 含义 | 示例 |
|------|------|------|
| `node-type T` | 过滤标签 | `(node-type Call)` |
| `child N P` | 第 N 个子节点满足 P | `(child 0 (node-type Variable))` |
| `has-child P` | 存在子节点满足 P | `(has-child (node-type LiteralInt))` |
| `= field value` | 字段相等 | `(= name "fact")` |
| `> field value` | 数值比较 | `(> (child-count) 1)` |
| `and P Q` | 与 | `(and (node-type Call) ...)` |
| `or P Q` | 或 | `(or (node-type Call) (node-type Lambda))` |
| `not P` | 非 | `(not (node-type LiteralInt))` |
| `exists P` | 子树中存在 | `(exists (node-type Call))` |
| `ref-count N` | 引用计数 | `(= (ref-count :node) 0)` |

### 4.3 查询执行引擎

```cpp
class QueryEngine {
    const FlatAST& ast_;
    const StringPool& pool_;

    // 执行查询，返回匹配的 NodeId 集合
    std::vector<NodeId> execute(const QueryExpr& q);

    // 解析查询表达式
    QueryExpr parse(std::string_view sexpr);
};
```

执行模型：**filter + map**，直接在 FlatAST SoA 上迭代。

---

## 5. 变换层 (Mutation Layer)

### 5.1 Patch Generation

查询结果 → 结构化补丁：

```cpp
struct Patch {
    NodeId target;          // 目标节点
    PatchOp op;             // 操作类型
    uint32_t field;         // 字段偏移
    uint64_t new_value;     // 新值 (NodeId / SymId / int64)
};

enum class PatchOp {
    SetTag,       // 改节点类型
    SetIntValue,  // 改字面量值
    SetSymId,     // 改符号名
    SetChild,     // 改子节点引用
    InsertChild,  // 插入子节点
    RemoveChild,  // 删除子节点
    ReplaceNode,  // 替换整个子树
};
```

### 5.2 Transform DSL

```
;; 替换特定子树
(query-and-transform
  (node-type Call)
  (= (child 0 (node-type Variable) (= name "+")))
  (child 1 (node-type LiteralInt))
  (child 2 (node-type LiteralInt))
  → (transform (fold-constants :match)))

;; 自动修复: (+ x x) → (* x 2)
(query-and-fix
  (and (node-type Call)
       (child 0 (= sym_id "+"))
       (= (child 1) (child 2)))
  (fix (replace-with
         (Call Variable["*"]
               (child 1)
               (LiteralInt 2))))
```

### 5.3 Patch Application

复用 `apply_patches()` 接口（已在 Phase 2 实现）：

```cpp
bool apply_patches(FlatAST& ast, std::span<const Patch> patches);
```

---

## 6. 热更新 (Hot Swap)

### 6.1 函数级替换

```cpp
struct HotSwapRequest {
    NodeId old_function;     // 旧函数定义
    NodeId new_function;     // IR 中替换
};

bool hot_swap(IRModule& ir, HotSwapRequest req) {
    // 1. 找到旧函数的所有调用点
    auto callers = find_callers(ir, old_function);
    // 2. 更新所有调用点指向新函数
    for (auto& caller : callers)
        patch_call_target(caller, new_function);
    // 3. 触发热更新（下次调用生效）
    return true;
}
```

### 6.2 子树级替换

对于增量编译场景：

```cpp
// 只重编译改变的子树
IncrementalResult recompile(FlatAST& old_ast, span<const Patch> changes) {
    // 1. 应用 patch
    apply_patches(old_ast, changes);
    // 2. 找到受影响的子树
    auto affected = find_affected_subtrees(old_ast, changes);
    // 3. 只重编译受影响的子树
    for (auto root : affected) {
        auto new_ir = lower_subtree(root, old_ast);
        hot_swap(ir_module, {old_func, new_ir});
    }
    return {success, recompiled_count};
}
```

---

## 7. AI Agent 接口

### 7.1 C++ API

```cpp
class AuraQueryEngine {
public:
    // 构建索引（一次扫描）
    void build_index(const FlatAST& ast);

    // 查询
    QueryResult query(std::string_view query_sexpr);

    // 查询 + 变换
    TransformResult query_and_transform(
        std::string_view query_sexpr,
        std::string_view transform_sexpr);

    // 自动修复（错误 → 查询 → 修复 → 验证）
    FixResult auto_fix(CompileError err);

    // 热更新
    bool hot_swap(HotSwapRequest req);
};
```

### 7.2 AI 调用模式

```python
# Python/Agent 伪代码
engine = AuraQueryEngine(compiler_service)

# 查询
calls = engine.query("(node-type Call)")
for call in calls:
    print(f"Call at node {call.id}")

# 自动修复
result = engine.query_and_fix(
    "(and (node-type Call) (= (child 0) (child 1)))",
    "(fix (replace-with (Call [+, (child 0), (LiteralInt 2)])))"
)

# 热更新
engine.hot_swap({
    "old_function": "fact_v1",
    "new_function": "fact_v2"  # 新编译的 IR
})
```

### 7.3 --query CLI 模式

```
$ echo '(* 2 3)' | ./aura --query '(node-type LiteralInt)'
2 nodes match:
  node[1]: LiteralInt(2)
  node[2]: LiteralInt(3)
```

---

## 8. 与 M1 组件的关系

```
M1 组件                       M2 使用方式
────────────────────────────────────────────────────
FlatAST (SoA)                索引层直接迭代 SoA, 零拷贝
StringPool (SymId)           SymRefIndex 倒排
IRModule                     IRIndex + 热更新
PassManager                  作为变换后验证管线
CompilerService              M2 的宿主, 提供 arena / evaluator
apply_patches()              变换层的核心执行器
```

---

## 9. 实现路线图

### Phase 1: 查询引擎（~3天）

```
Day 1:
  └── ASTIndex: by_tag, by_child, by_sym_id 过滤
      (直接在 FlatAST SoA 上 std::views::filter)

Day 2:
  └── QueryEngine: query("(node-type Call)") 可解析 + 执行
      支持: node-type, child, =, and, or, not

Day 3:
  └── CLI --query 模式 + 初步测试
      SymRefIndex 构建
```

### Phase 2: 变换引擎（~2天）

```
Day 4:
  └── Patch 生成 + 应用集成
      Transform DSL 解析

Day 5:
  └── query-and-transform 管线闭环
      Tests: 模式匹配 + 替换
```

### Phase 3: 热更新 + AI 接口（~2天）

```
Day 6:
  └── hot_swap: 函数级 IR 替换
      recompile_subtree: 增量重编译

Day 7:
  └── auto_fix: 错误→查询→修复管线
      --query CLI 最终化
```

---

## 10. 开放问题

1. **查询性能**: 对 10K+ 节点的 AST，是否需要预构建倒排索引？Phase 1 先做迭代式过滤，性能不足再加索引。
2. **Transform DSL 语法**: 当前方案用 Lisp S-表达式，保持 homoiconic。是否需要更接近人类的语法（如 `find Call where ...`）？暂定 S-表达式，后续可加语法糖。
3. **热更新安全**: 函数替换时，正在执行的调用怎么办？Phase 3 时讨论。
4. **与 AuraQuery 的关系**: 设计仓库 `docs/aura_query.md` 定义了更完整的查询 DSL，M2 第一期只实现子集。

---

> 本文档是 M2 起始设计，版本 v0.1。实现中根据实际反馈迭代完善。
