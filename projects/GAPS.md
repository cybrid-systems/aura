# Core Gaps — 项目发现的核心短板

按发现顺序累积。修复后移至 "已修复" 区段。

---

## 待修复

### G9: `drop` C++ prim 在树遍历求值器中总是返回第一个参数

- **发现于:** kv-store LRANGE 实现 (2026-05-30)
- **严重度:** 🟠
- **场景:** `(drop 0 '(a b c))` 返回 `0` 而非 `(a b c)`
- **原因:** `eval_ir` 路径下 `drop` 的原语分派有问题。`take` 接口相同却正常。
  通过 `apply` 调用或 `--ir` 模式都正常，仅直接调用时异常。
- **绕过方式:** kv.aura 中使用 Lisp 层 `kv-drop`（纯递归实现）
- **状态:** ⬜ 待修（根因在 IR evaluator 的原语分派机制）

### G10: `take`/`drop` 在 n=0 时返回空/0 而非原列表

- **发现于:** kv-store LRANGE 实现 (2026-05-30)
- **严重度:** 🟡
- **场景:** `(take 0 '(a b c))` 应返回 `'()` 而非 void
- **修复:** C++ `take` 加了 n==0 early return
- **状态:** ✅ `take` 已修，`drop` 见 G9

### G7: `member` 使用 `eq?` 而非 `equal?` 比较元素

- **发现于:** kv-store TTL 测试 (2026-05-30)
- **严重度:** 🟡
- **场景:** `(member "keep" (list "keep" "other"))` 返回 0 而非找到的尾列表
- **原因:** `member` prim 中用 `==`（原始 EvalValue），字符串同值不同副本时比较失败
- **绕过方式:** 用 `equal?` 自定义查找函数
- **状态:** ✅ 已修（改用 content-aware 比较）

---

## 已修复

### G1: `make-hash-table` 不存在，正确名称是 `hash`

- **发现于:** kv-store (2026-05-30)
- **严重度:** 🟢（文档问题）
- **修复:** 使用 `(hash)` 替代
- **状态:** 已适配 ✅

### G5: `make-void` / `void` prim 缺失

- **发现于:** kv-store (2026-05-30)
- **严重度:** 🟡
- **修复:** `src/compiler/evaluator_impl.cpp` 中注册 `(void)` 原语
- **状态:** 已修复 ✅

### G6: `load` 原语缺失

- **发现于:** kv-store (2026-05-30)
- **严重度:** 🟠
- **修复:** `src/compiler/evaluator_impl.cpp` 中注册 `(load)` 原语
- **状态:** 已修复 ✅

### G8: `current-time` prim 缺失

- **发现于:** kv-store TTL 需求 (2026-05-30)
- **严重度:** 🟠
- **修复:** `src/compiler/evaluator_impl.cpp` 中注册 `(current-time)` 原语
- **状态:** 已修复 ✅
