# C1: IR-level Import — 设计文档

**目标**：消除 `(require std/list)` 的 tree-walker fallback，使 `require` + 后续调用全走 IR。

---

## 现状

当前 `require`/`import` 导致 `needs_tree_walker_fallback` 返回 true，整个表达式降级到 tree-walker。

```
输入: (require std/list all:) (foldl + 0 (list 1 2 3))
     ↓ eval() 检测到 require → 全 tree-walker
     ↓ compile_module 加载 std/list → ir_cache_ 填充
     ↓ tree-walker 执行 foldl → 正确但慢
```

**问题**：`require` 只是副作用（加载模块、填充 env 和 cache），后续表达式完全可以走 IR。

---

## 设计

### 核心思路：Pre-eval require, IR for the rest

在 `eval()` 中，三步走：

```
输入: (require std/list all:) (foldl + 0 (list 1 2 3))
     ↓ Step 1: Scan top-level for require/import
     ↓ Step 2: Execute require via tree-walker (side effects)
               → compile_module → ir_cache_["range"] = ... 
               → env: range/foldl/sort bound
     ↓ Step 3: Lower & execute the body via IR
               → (foldl + 0 (list 1 2 3))
               → foldl/range in ir_cache_ → IR path
```

### 修改点

**1. `CompilerService::eval()`** — 新增 require pre-scan

```cpp
EvalResult eval(string_view input) {
    // ... parse + macro expand ...

    // NEW: Pre-scan and execute top-level require/import
    auto stripped = pre_exec_requires(*flat_ptr, *pool_ptr, expanded_root);
    // stripped = expression with require statements removed
    // (require evaluated as side effect → env + ir_cache_ updated)

    if (needs_tree_walker_fallback(...)) {
        return evaluator_.eval_flat(...);
    }

    // IR path — cached functions available
    auto ir_mod = lower_to_ir_with_cache(...);
    // ...
}
```

**2. `pre_exec_requires()`** — 新函数

```cpp
// Returns the body node ID with require statements removed.
// Executes each require via tree-walker as side effect.
NodeId pre_exec_requires(FlatAST& flat, StringPool& pool,
                          NodeId root, Evaluator& eval) {
    if (root == NULL_NODE) return root;
    auto v = flat.get(root);
    
    // (require ...) — standalone
    if (is_require(v)) {
        exec_require(flat, pool, root, eval);
        return NULL_NODE;  // no body left
    }
    
    // (begin (require ...) (expr1) (expr2))
    if (v.tag == NodeTag::Begin) {
        vector<NodeId> remaining;
        for (auto c : v.children) {
            auto cv = flat.get(c);
            if (is_require(cv)) {
                exec_require(flat, pool, c, eval);
            } else {
                remaining.push_back(c);
            }
        }
        if (remaining.empty()) return NULL_NODE;
        if (remaining.size() == 1) return remaining[0];
        // Rebuild begin with non-require children
        return rebuild_begin(flat, remaining);
    }
    
    return root;  // no require → unchanged
}
```

**3. `is_require()` 检查**
```cpp
bool is_require(NodeView v) {
    if (v.tag != NodeTag::Call) return false;
    auto callee = v.child(0);
    if (callee == NULL_NODE) return false;
    auto cv = flat.get(callee);
    if (cv.tag != NodeTag::Variable) return false;
    auto name = pool.resolve(cv.sym_id);
    return name == "require" || name == "import" || name == "use";
}
```

**4. `exec_require()`** — 调用 tree-walker 执行 require
```cpp
void exec_require(FlatAST& flat, StringPool& pool,
                   NodeId node, Evaluator& eval) {
    // eval via tree-walker → triggers compile_module
    // → fills ir_cache_ + env bindings
    eval.eval_flat(flat, pool, node, eval.top_env());
}
```

### 后续影响

- `require` 返回 void → 从表达式中移除后不影响结果
- `all:` 导入将 bare names 加入 env，但**不会**自动加入 `ir_cache_`
  - 非 `all:` 导入（带前缀如 `list:range`）的调用在 `needs_tree_walker_fallback` 中会触发 fallback
  - 如果目标是全 IR 消除 fallback，需要额外追踪 `all:` 绑定的名字

### 边界情况

| 场景 | 处理 |
|------|------|
| `(require std/list) (range 0 10)` | require 走 tree-walker，range 走 IR（prefix 可用） |
| `(require std/list all:) (foldl + 0 xs)` | require 走 tree-walker，`all:` 名字入 `user_bindings_` → 触发 fallback（当前限制） |
| `(if cond (require std/a) (require std/b))` | require 在 if 分支内——不处理，保持 tree-walker fallback |
| `(define (f) (require std/foo) (foo))` | require 在函数体内——不处理，保持 tree-walker fallback（cache_define 已检测 require 并跳过 IR 缓存） |

---

## 实现步骤

1. **`pre_exec_requires()`** — 在 `eval()` 中扫描、执行 require、返回去 require 的 body
2. **`needs_tree_walker_fallback` 更新** — 移除 `require`/`import` 从 tree_walker_only 集合（因为已经被 pre-scan 处理）；保留对非 top-level require 的 fallback
3. **测试** — `(require std/list all:) (foldl + 0 (range 1 10))` 验证 IR 路径
4. **Benchmark** — 对比 before/after 的 stdlib 使用性能

### 不做的

- ❌ IR 级 `import` 原语（不走 IR opcode）
- ❌ 函数体内 require 支持（保持现有 fallback）
- ❌ `all:` 导入的自动 `ir_cache_` 注册

---

## 时间估算

| 步骤 | 估计 | 说明 |
|------|------|------|
| 1. `pre_exec_requires` | 2h | 核心逻辑 |
| 2. `needs_tree_walker_fallback` 更新 | 1h | 移除 tree_walker_only require/import 等 |
| 3. 测试 | 1h | 集成 + bash |
| 4. Benchmark | 1h | before/after |
| **总计** | **5h** | |

---

## 验收标准

```bash
# Before: 全部走 tree-walker fallback
(require std/list) (foldl + 0 (list 1 2 3 4 5))  → 15  ✅ IR path

# Stdlib functions now cached → subsequent calls faster
(require std/list) (foldl + 0 (range 1 100))       → 4950 ✅ IR path

# Complex pipelines
(require std/list all:) (sort (list 3 1 4 1 5) <)  → (1 1 3 4 5) ✅

# No regressions
python3 build.py check                               → all pass
```
