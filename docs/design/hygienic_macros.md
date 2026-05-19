# C3: Hygienic Macros — 设计文档

**目标**：Ghuloum Step 16 — 使 defmacro 不会意外捕获上下文中的变量。

---

## 现状

当前 `defmacro` 是简单的模板替换：

```scheme
(defmacro (swap! a b)
  (let ((tmp a)) (set! a b) (set! b tmp)))
```

展开 `(swap! x y)` 时，`tmp` 是模板引入的名字。如果在调用点 `tmp` 已被绑定，则发生意外捕获：

```scheme
(let ((tmp 42)) (swap! x y) tmp)  ;; tmp 被 macro 覆盖！
```

## 方案：自动 gensym 宏引入的绑定

每次展开宏时，对模板中 **不是宏参数** 的绑定名字自动 gensym。

### 实现：`define-hygienic-macro`

新特殊形式，行为同 `defmacro`，但在 `clone_macro_body` 中增加自动重命名：

```
展开过程：
1. clone_macro_body 递归遍历模板
2. 遇到 Variable:
   - 是宏参数? → 用用户参数替换
   - 不是宏参数? → 检查是否为绑定位置（let/lambda/define）
   - 是绑定位置? → gensym 新名字 ↓
   - 是引用位置? → 检查是否引用了一个被 gensym 的绑定
     - 是 → 使用 gensym 后的名字
     - 否 → 保留原名（可能是 built-in）

3. 绑定位置: let sym_id, lambda params, define sym_id
4. 引用位置: 任何 Variable（不在绑定位置）
```

### 修改点

1. `clone_macro_body()` 增加 `name_map` 参数（template_name → fresh_name）
2. Let/Lambda/Define 绑定处理：自动 gensym 非参数名字
3. Variable 引用处理：查 name_map 做重命名
4. `macro_expand_all()` 调用时为 hygiene macro 传入 name_map
5. 跟踪内置名字集合（如果/cond/let/define/lambda/begin/set!/quote/+/-/etc.），不重命名

### 时间估算

| 步骤 | 估计 |
|------|------|
| `clone_macro_body` 增加 name_map | 1h |
| Let/Lambda/Define 绑定位置处理 | 1h |
| Variable 引用重命名 | 0.5h |
| `define-hygienic-macro` 注册 | 0.5h |
| 测试 | 1h |
| **总计** | **4h** |
