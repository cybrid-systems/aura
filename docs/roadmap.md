# Aura — 路线图

## 当前状态（2026-05-18 更新）

**核心完成度：可写 500+ 行业务代码** ✅

### 已实现

```
✓ 语言核心：100+ 原语、TCO、lambda、let/let*/letrec、cond、match、when/unless
✓ 宏系统：quasiquote、gensym、递归展开、dotted rest param
✓ 模块命名空间：require/import prefix、export、循环检测、自动 lib 发现
✓ 渐进类型：L6 推断、forall、Float、增量 typecheck（dirty skip + type cache）
✓ 错误处理：try/catch/raise/assert、原语返回 error
✓ 测试框架：check/check=、test-suite、run-tests
✓ 正则：regex-match? regex-find regex-replace regex-split
✓ 数学：sin cos tan asin acos atan log log10 exp pow sqrt floor ceil round
✓ 文件 IO：read-file write-file file-copy file-delete file-size file-exists? directory-list
✓ EDSL：set-code query:* mutate:* typecheck-current eval-current 15+ 原语
✓ 格式化：--fmt / --fmt -i / --fmt --check
✓ AI Agent：DeepSeek 默认 + 双阶段工作流 + 自动测试 + truncation 检测
✓ 标准库：list math string json struct validate test（8 lib）

=== 本周新增（5/17 高密度开发）===
✓ IR 管线默认启用：eval() 统一走 IR-first + tree-walker fallback
✓ 原语桥接：Variable 降级检查 Primitives 表，全量原语 IR 可调用
✓ Bool 语义统一：ConstBool opcode，比较/逻辑返回 make_bool
✓ 链式比较修复：pairwise AND 替代错误布尔 chaining
✓ 常量折叠布尔感知：replace_bool 保持 bool 类型
✓ 闭包桥接：IRClosure ↔ tree-walker Closure 互通
  → map/filter/foldl + lambda-作为参数 全程走 IR
✓ Pair/Quote IR 降级：递归 cons chain 替代 Quote fallback
  → '(), '(a b c), '(a . b), '((a b) (c d)) 全部 IR 可执行
✓ ConstVoid：'() 空列表正确表示为 void
✓ 函数热替换 + 依赖追踪（invalidate_function）
✓ 增量编译：dirty 标记 + 类型缓存
✓ EDSL 新增：query:pattern, query:node-type, mutate:insert-child

=== 5/18 新增（持续增量演进）===
✓ ArenaGroup 多模块管理：compile_module / unload_module / reload_module
✓ 模块级增量编译：ModuleState dirty 追踪 + mark_module_dirty + 自动重编
✓ 磁盘缓存 (mmap)：write_cache/open_cache 集成到 compile_module
✓ --serve 新增 module 命令 (compile/unload/reload/list/stats)
✓ 设计文档全面对齐（caas_integration / roadmap / design 仓库）
```

### 测试

```
Bash 回归：    106 tests  ✅
C++ test_ir：  61 cases    ✅
集成管线：     50+ cases   ✅
```

### 代码统计

```
src/core/       ~2,500 行  — FlatAST、arena、type
src/parser/     ~1,200 行  — lexer + parser
src/compiler/   ~9,500 行  — evaluator + IR + passes + bridge + EDSL
lib/std/        8 files    — ~300 行 Aura 代码
tests/          C++ 61 + bash 106 + AI Agent
```

### Fellback（有意保留，未来逐步消除）

| 触发条件 | 原因 |
|----------|------|
| `import`/`use`/`require` | 环境绑定副作用（IR 无法复制） |
| EDSL `query:`/`mutate:` | 需要求值器内部状态 |
| 特殊形式 `try`/`catch`/`when`/`unless` | 没有对应 IR 指令 |

## 下一步

### P0 ✅ — 语言打磨（全部完成）

| 项 | 说明 | 状态 |
|----|------|------|
| EDSL 自动修复 | 运行时错误 → set-code → query → mutate → eval-current | ✅ 完成 |
| Agent history truncation | 只保留最近 3 轮避免窗口爆炸 | ✅ 完成 |
| Agent 截断检测 | `finish_reason == "length"` 检测 + 警告 | ✅ 完成 |
| `string->number` 非数字返回 #f | 符合 Scheme 语义 | ✅ 完成 |
| `when`/`unless` 特殊形式 | Scheme 兼容 | ✅ 完成 |
| Agent DONE 验证 | 编译后 auto-test 通过才接受 DONE | ✅ 完成 |

### P1 ✅ — 功能扩展（全部完成）

| 项 | 说明 | 状态 |
|----|------|------|
| IO 库 | file-copy, delete, size, directory-list | ✅ 完成 |
| Regex 库 | match? find replace split | ✅ 完成 |
| Math 库 | sin cos tan log exp pow sqrt floor ceil round | ✅ 完成 |
| 函数缓存 + 依赖追踪 | ir_cache_ + 自动 re-lower | ✅ 完成 |
| 标准库扩充 | list math string json struct validate test | ✅ 完成 |

### P2 — 性能 & 基础设施

| 项 | 说明 | 优先级 | 状态 |
|----|------|--------|------|
| IR 管线默认 | eval() 统一 IR-first + fallback | 🟢 | ✅ **完成** |
| 原语桥接 + 闭包桥接 | Primitive 值加载 + map/filter IR-closure 互通 | 🟢 | ✅ **完成** |
| Bool/Quote/Pair IR 覆盖 | ConstBool, ConstVoid, 递归 cons chain | 🟢 | ✅ **完成** |
| ArenaGroup 集成 | compile_module / unload_module / reload_module | 🟢 | ✅ **5/18** |
| 模块级增量编译 | ModuleState dirty 追踪 + mark_module_dirty | 🟢 | ✅ **5/18** |
| 磁盘缓存 (mmap) | write_cache/open_cache 集成到 compile_module | 🟢 | ✅ **5/18** |
| Pair IR 指令优化 | 原生 MakePair/Car/Cdr opcode 替代 cons 调用 | 🟡 | ⬜ |
| muddy skip 深度优化 | per-module skip | 🟡 | ⬜ |
| IR 级 import/模块 | 消除模块 fallback | 🔴 | ⬜ |
| try/catch IR 支持 | 异常 IR 指令 | 🔴 | ⬜ |
| LLVM JIT | `--jit` 模式编译到原生代码 | 🔴 | ⬜ |
| AOT 编译 | 静态二进制 | 🔴 | ⬜ |
| 自举 | 用 Aura 写 Aura 编译器 | 🔴 | ⬜ |

### P3 — 类型检查 & 语言修复（5/18 启动）

| 项 | 说明 | 工作量 | 状态 |
|----|------|--------|------|
| 运行时 coercion | IR Interpreter Add/Sub/Mul/Div 处理 string→int | ~1h | ⬜ |
| consistent_unify | String~Int 渐进类型一致性 | ~1h | ⬜ |
| arith 类型推断 | `(+ "42" 1)` 返回 Int 而非 String | ~1h | ⬜ |
| 类型检查 exit code | err_arity/err_type 返回 exit 1 | ~1h | ⬜ |
| 递归函数 IR 缓存 | cache_module 跳过递归函数的修复 | ~2h | ✅ **5/18** |
