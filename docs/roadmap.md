# Aura — 路线图

---

## 里程碑

```
M1  求值器                      ✅ 纯 FlatAST 管线
M2  查询引擎                    ✅ query:* 原语
M3  语言体系                    ✅ 宏/反射/类型/工具链/标准库
M4  高级特性                    ✅ AI 闭环/ABF 缓存/模块/Typed Mutation
M5  模块命名空间 v2             ✅ use/module-get/export/prefix
P6  增量编译                    ✅ dirty 跟踪 + 增量 typecheck
P7  错误处理                    ✅ try/catch/raise + error 原语
P8  测试框架                    ✅ check/check=/test-suite + bash runner
P9  格式化                      ✅ --fmt / --fmt -i / --fmt --check
P10 生产后端                    ⬜ LLVM JIT / AOT / 自举
```

## 当前能力

| 领域 | 状态 |
|------|------|
| 语言核心 | ✅ ~80 原语 + TCO + match + define-struct |
| 标准库 | ✅ 7 libs (list/math/string/json/struct/validate/test) |
| 模块系统 | ✅ 前缀注入 + 导出控制 + 循环检测 + 自动 lib 发现 |
| 增量 typecheck | ✅ dirty-aware，跳过 clean 子树，持久 TypeRegistry |
| 错误处理 | ✅ try/catch/raise，原语返回 error 不崩溃 |
| 测试 | ✅ check/check= + bash runner (80+ tests) |
| 格式化 | ✅ --fmt / --fmt -i / --fmt --check |
| EDSL | ✅ 15+ query/mutate 原语 + workspace 模型 |
| AI Agent | 🟡 MiniMax/GPT/DeepSeek 通用；prompt 待打磨 |
| IR 管线 | 🟡 IR executor 可用但非默认路径 |

## 代码统计

```
C++ 源文件: 33         Core C++ 行: ~12500
Aura 标准库: 7 files   Aura 代码: ~270 行
测试: C++ 22 + bash 80+     原语: ~80
```

## 下一步

### P0 — 让 500 行项目可写

| 项 | 工作量 | 影响 |
|----|--------|------|
| `run-tests` 完善 | ~20 行 C++ | 🔴 LLM 自测闭环 |
| 标准库补全 | ~100 行 Aura | 🟡 按需 |
| quasiquote + 特殊形式 splicing 修复 | ~50 行 parser | 🟡 宏系统完整 |

### P1 — 让 1000+ 行项目可维护

| 项 | 说明 |
|----|------|
| Incremental dirty skip 深度优化 | 当前 per-node，可加 per-module skip |
| export 循环依赖检测激活 | 代码已就绪，等待多模块场景触发 |
| IR 管线默认启用 | IR executor 已存在，但 99% 走树遍历器 |

### 极远期

```
LLVM JIT 降级    — —jit 模式编译到原生代码
AOT 编译         将 Aura 代码编译为静态二进制
自举             用 Aura 写 Aura 编译器
```
