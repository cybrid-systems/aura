# 结构化变异 API —— Issue #11 / 后续

> **Status (2026-06-07, Issue #112):** ✅ 已大幅扩展。当前 12 个 mutate
> 原语，覆盖精确节点修改、模式替换、高层重构。原子性、回滚、并发安全
> 已在 #107 / #108 / #109 / #110 中实装。
>
> **Audience:** 写 mutate primitive 的开发者（详见 [`docs/developer/evaluator.md`](../developer/evaluator.md)）
> **Audience (2):** 使用 mutate 原语的 Agent 开发者

---

## 1. 原子性与回滚

每个 `mutate:*` 原语都是**原子的**：

- 整段操作在 `workspace_mtx_` 的 `unique_lock` 下完成（[#107]）。
- 失败时通过 `add_mutation_with_rollback` 记录到 `mutation_log_`，可
  `ast:rollback <mutation-id>` 撤销（[#107 part 1]）。
- 失败时返回 tagged pair `("error" . ("kind" . "message"))`（"bad-arg" /
  "no-workspace" / "out-of-range" / "type-mismatch" / "read-only"）。

```scheme
(define snap (ast:snapshot "before-try"))
(mutate:rebind "fib" "(lambda (n) (* n 2))" "linearize")
(if (= 0 (eval-current-output))
  (begin
    (display "regression!")
    (ast:restore snap))
  (display "ok, keep changes"))
```

### Panic checkpoint（深度 mutate 安全网）

对于跨多个 mutate 的复杂重构，使用 `save-panic-checkpoint` /
`restore-panic-checkpoint`：

```scheme
(save-panic-checkpoint)
(mutate:rename-symbol "x" "y" "rename x to y")
(mutate:extract-function 5 "helper" "extract helper")
(restore-panic-checkpoint)  ; 出错时回退
```

底层走 `Evaluator::save_panic_checkpoint` / `restore_panic_checkpoint`
（`evaluator_impl.cpp` ~17906），用 `std::stack` 维护 panic-safe 的
栈底 + 异常时 unwind 到该点。

---

## 2. Mutate 原语完整列表（2026-06）

### 2.1 精确节点修改

| 原语 | 用途 | 复杂度 |
|------|------|--------|
| `mutate:replace-value` | 按 NodeId 替换节点值（int/float/string/auto） | O(1) + mutation log |
| `mutate:replace-type` | 按 NodeId 替换类型注解 | O(1) + mutation log |
| `mutate:tweak-literal` | 微调字面量值（n → n + delta） | O(1) |
| `mutate:record-patch` | 记录一个 patch（不影响 AST） | O(1) |
| `mutate:remove-node` | 按 NodeId 断开子树 | O(1) |
| `mutate:insert-child` | 在父节点 position 插入子树 | O(N)（N = 父子树大小）|

### 2.2 函数级修改

| 原语 | 用途 | 复杂度 |
|------|------|--------|
| `mutate:rebind` | 按函数名替换完整定义 | O(找到 + 重 build) |
| `mutate:set-body` | 替换函数体（保留参数签名） | O(1) |

### 2.3 模式替换

| 原语 | 用途 | 复杂度 |
|------|------|--------|
| `mutate:replace-pattern` | 模式匹配 + 替换（带 `...` 通配符） | O(Nodes × PatternDepth) |
| **`mutate:query-and-replace`** | 组合 `query:where` + 模板替换 | O(Nodes × Predicates) |

### 2.4 高层重构

| 原语 | 用途 | 复杂度 |
|------|------|--------|
| `mutate:rename-symbol` | 重命名符号（def + use 自动遍历） | O(uses × distance) |
| `mutate:extract-function` | 提取子树为新顶层函数（自动收集自由变量） | O(subtree × free-vars) |
| `mutate:inline-call` | 内联调用（body 替换 call site） | O(body) |
| `mutate:move-node` | 移动子树到新父节点指定位置（带循环检测） | O(子树 detach + insert) |
| `mutate:wrap` | 在父位置加一层（包裹） | O(子树) |
| `mutate:splice` | 批量插入多个子节点 | O(插入数) |
| `mutate:refactor/extract` | `mutate:extract-function` 的 alias | O(同上) |

---

## 3. 各原语详细语义

### 3.1 `mutate:rename-symbol`

```scheme
(mutate:rename-symbol old-name new-name [summary]) → #t/#f
```

AST 中所有同名 symbol 的 def + use 节点。支持 Variable、Define、DefineType、
DefineModule、Let、LetRec、Set、MacroDef。

```scheme
(set-code "(define (square x) (* x x))")
(eval-current)
(mutate:rename-symbol "square" "double") → #t
(double 5) → 25
```

**实现位置：** `evaluator_impl.cpp` ~10445

### 3.2 `mutate:extract-function`

```scheme
(mutate:extract-function node-id new-name [summary]) → (define-id . call-id)
```

将子树提取为新的顶层函数定义。自动分析自由变量（过滤 builtin），生成带
参数的 lambda，原地替换为函数调用。新定义插入 workspace root 首位。

```scheme
;; 提取 (* x 3) → mul3
(set-code "(define (calc x) (+ (* x 3) 1))")
(eval-current)
(mutate:extract-function <mul-call-id> "mul3") → (define-id . call-id)
(eval-current)
(calc 2) → 7    ; 原始函数不变
(mul3 5) → 15   ; 新函数: (define (mul3 x) (* x 3))
```

**实现位置：** `evaluator_impl.cpp` ~10579

### 3.3 `mutate:inline-call`

```scheme
(mutate:inline-call call-node-id [summary]) → #t/#f
```

将函数调用原地内联：克隆函数体，将形式参数替换为实际参数。
支持命名函数和内联 lambda。被内联函数的定义必须在当前 AST 中。

```scheme
(set-code "(begin (define (f x) (+ x 1)) (define (g x) (f x)))")
(eval-current)
(mutate:inline-call <(f x) call-id>) → #t
(eval-current)
(g 5) → 6  ; g 的 body 现在直接是 (+ x 1)
```

**实现位置：** `evaluator_impl.cpp` ~10702

### 3.4 `mutate:move-node`

```scheme
(mutate:move-node node-id new-parent-id new-position [summary]) → #t/#f
```

将子树从当前位置移到新的父节点下指定位置。
自动做循环检测。支持跨父节点移动。

```scheme
(set-code "(begin (display 1) (display 2) (display 3))")
(eval-current)
(mutate:move-node <display(3)-id> <root-id> 0) → #t
(eval-current)  ; 输出 312
```

**实现位置：** `evaluator_impl.cpp` ~10503

### 3.5 `mutate:replace-pattern`

```scheme
(mutate:replace-pattern pattern-string replacement-string [summary]) → #t/#f
```

模式匹配替换：查找所有匹配模式的节点，替换为替换模板。
`...` 作为通配符，匹配任意单个子树并在替换中代入。

```scheme
;; 精确替换
(mutate:replace-pattern "(* 2 x)" "(+ x x)")

;; 通配符替换: 将任意除法包裹守卫
(mutate:replace-pattern "(/ ... ...)" "(if (= ?2 0) 0 (/ ?1 ?2))")
;; 注: ?1, ?2 为占位符，后续版本支持
```

### 3.6 `mutate:query-and-replace` (#110)

```scheme
(mutate:query-and-replace predicate-template [summary]) → #t/#f
```

把 `(query:where :field value)` 谓词 + 替换模板组合为单步原子操作。
`...` 在模板里是"被匹配节点的全部 children"占位符。

```scheme
;; 把所有 (+ ...) 替换为 (y ...)
(mutate:query-and-replace
  (query:where :callee "+")
  "y"
  "linearize adds")

;; 用模板做局部替换
(mutate:query-and-replace
  (query:where :callee "foo")
  "(bar ...)"
  "rename foo→bar")
```

**实装位置：** `evaluator_impl.cpp` ~4566（Issue #110）
**Closing doc：** [`docs/issue-closings/110-closing.md`](../issue-closings/110-closing.md)

> ⚠️ **使用警示**：在写一个 combine（read + write）循环的 mutate 时，
> **必须**在循环外 snapshot `end_id = flat.size()`。详见
> [`docs/developer/evaluator.md §1`](../developer/evaluator.md#1-the-self-modifying-flat-iteration-rule-issue-111-lesson)。

### 3.7 `mutate:wrap` / `mutate:splice`

```scheme
;; 在指定父位置加一层
(mutate:wrap node-id wrapper-code [summary])
;; 例: (mutate:wrap 3 "(display _)" "wrap")
;;   把 3 号节点的父位置改成 (display 3-子树)
;;   `_` 是占位符，被替换为原节点

;; 批量插入子节点
(mutate:splice parent-id position child-code... [summary])
;; 例: (mutate:splice 0 1 "(display 1)" "(display 2)" "insert")
;;   向 0 节点的子节点列表 position 1 位置插入多个新节点
```

**实现位置：** `evaluator_impl.cpp` ~10088 / ~9996

### 3.8 `mutate:rename-symbol` / `mutate:refactor/extract`（alias）

`mutate:refactor/extract` 是 `mutate:extract-function` 的 alias —— 早期
名字，保留向后兼容。

---

## 4. Mutate 协议总览

每个 `mutate:*` 的标准实现模式（详见
[`docs/developer/evaluator.md §3`](../developer/evaluator.md#3-mutate-primitives--locking-protocol)）：

```cpp
primitives_.add("mutate:foo", [this](std::span<const EvalValue> a) -> EvalValue {
    // 1. 取 unique_lock
    std::unique_lock<std::shared_mutex> wlock(workspace_mtx_);

    // 2. 检查 read-only
    if (workspace_read_only_) return mev("read-only", "workspace is read-only");

    // 3. yield at mutation boundary（让其他 fiber 跑）
    defuse_version_++;
    if (aura::messaging::g_fiber_yield_mutation_boundary)
        aura::messaging::g_fiber_yield_mutation_boundary();

    // 4. 验证参数
    if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]))
        return mev("bad-arg", "usage: (mutate:foo node-id summary)");

    // 5. 验证 workspace
    if (!workspace_flat_) return mev("no-workspace", "...");
    auto& flat = *workspace_flat_;
    auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
    if (node >= flat.size()) return mev("out-of-range", "...");

    // 6. 应用修改（snapshot flat.size() 如果会增长）
    const auto end_id = flat.size();
    for (aura::ast::NodeId id = 0; id < end_id; ++id) { ... }

    // 7. 记录 mutation + mark dirty + 触发 defuse touch
    auto mid = flat.add_mutation_with_rollback(...);
    workspace_flat_->mark_dirty_upward(node);
    defuse_affected_syms_.insert(name);
    if (defuse_touch_fn_) defuse_touch_fn_(defuse_index_, sym);

    // 8. 返回
    return make_int(mid);
});
```

### 错误返回的 `mev` helper

```cpp
auto mev = [this](const std::string& k, const std::string& m) -> EvalValue {
    auto mi = string_heap_.size(); string_heap_.push_back(m);
    auto ki = string_heap_.size(); string_heap_.push_back(k);
    auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
    auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
    return kp;
};
```

产生 canonical `("error" . ("kind" . "message"))` shape。

---

## 5. 语义保持与回归保护

每个原语在 mutate 后验证：

- `typecheck-current` 通过
- `eval-current` 结果与 mutate 前一致（对函数调用参数做 reference oracle 比对）
- AST node id 在 mutate 前后保持稳定（除新增/删除节点外）

### 测试覆盖

- `tests/suite/safe-refactor.aura` —— 19 个 suite 测试
- `tests/fuzz_defuse.py --quick` —— 200/200（def-use 链不变性）
- `tests/fuzz_workspace.py --quick` —— 290+/290+（mutate → eval 序列）
- `tests/fuzz_equiv_mutate.py` —— 等价性 fuzz（mutate + eval 与原始 eval 结果一致）
- `tests/fuzz_snapshot.py --quick` —— 405/405（snapshot 兼容 + 不可变 history）

ASAN: 0 leaks on 50-iter snapshot+mutate+restore loop。

---

## 6. 实现细节（#11 时代遗留）

- `FlatAST` 新增 `add_raw_node()` 和 `parent_of()` 公共 API
- 修复 `add_lambda` / `add_call` 漏掉的 `link_children` 调用
- 修复 `set_child` / `insert_child` / `remove_child` 的 `parent_` 维护
- 修复 Begin 评估器中 NULL_NODE children 的容错

---

## 7. Future Work

- 🟡 **增量类型检查**：mutate 后只类型检查 dirty 子树（当前是全量 ~1-5ms，足够）
- 🟡 **mutation log Aura API**：暴露给 Aura 端做复杂回滚
- 🟡 **AutoFixEngine**：基于规则的自动修复（`std/rule.aura` 的更高级集成）
- 🟡 **编辑器预览模式**：mutate 但不实际改 AST，先在 shadow tree 上跑
