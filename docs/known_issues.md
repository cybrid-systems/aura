# Aura 已知问题

> 持续跟踪的已知 bug 和设计限制。

## 🔴  REPL 模式 read-line 阻塞行为

REPL 交互模式下 `read-line` 与 `display` 混合使用时，输出顺序可能不符合预期。
这是因为 `display` 输出到 stderr 而 `read-line` 读取 stdin。

**影响**：交互式 REPL 体验不完美。

## 🟡  `(quote ())` 返回 0

空列表的 quote 形式 `'()` / `(quote ())` 返回值是 `0` 而非 `()`。
这是因为空列表的哨兵值在处理中存在一致性缺陷。

**影响**：
- `'()` 不能作为空列表字面量使用
- 需要用 `(list)` 构造空列表

**隔离**：所有标准库和示例代码都使用 `(list)` 而非 `'()`，不受影响。

## 🟡  eval_data_as_code 宏展开路径限制

宏展开后重新求值（`eval_data_as_code`）在某些嵌套场景下可能丢失正确的
环境引用，导致闭包变量查找失败。

**影响**：复杂宏模式（如 DSL 生成带有闭包绑定的代码）可能有问题。

## 🟢  standard library 待办

以下功能可在标准库中实现，暂未列入：

- `number->string` — 数字转字符串（当前只能用 `display` 输出）
- `string->list` / `list->string` — 在 std/string 中已有别名
- `odd?` / `even?` — 已添加 ✅
- `string-split` 字符串分隔符 — 已支持 ✅

## 🟢  `procedure?` 对基元返回 #t

已修复 ✅ — `(procedure? +)` 返回 `#t`（2026.05.16）

## ✅ 已修复（历史记录）

以下问题已修复，仅作存档：

- **闭包按值捕获环境快照**（2026.05.16 — letrec 两阶段求值）
- **`"` 在字符串中不支持转义**（2026.05.16 — lexer 支持 `\"`）
- **defmacro 点列表 `(name . args)`**（2026.05.16 — parser 支持）
- **类型谓词返回 int 而非 bool**（2026.05.16 — 全改为 `make_bool`）
- **`--serve` 下 JSON 协议被 display 打乱**（2026.05.16 — display 输出改 stderr）
- **`+` 等基元不能作为值传递**（2026.05.16 — PrimitiveRef 支持）
- **Pipe 模式 `;` 注释报错**（2026.05.16 — 分割器识别行注释）
- **build_dbg 误提交到 git**（2026.05.16 — 历史重写移除）
