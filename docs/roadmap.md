# Aura 路线图

**更新：2026-05-23 晚**

---

## 当前能力

| 维度 | 状态 |
|------|:----:|
| 编译器 | 0 crash fuzz (4800+ cases), 3 套 fuzz, ASan/UBSan 通过 |
| 类型系统 | Sound Gradual Typing, ADT + match 穷尽性, let-poly, blame |
| M4 线性所有权 | move/borrow/drop 编译期 + 运行时检测 |
| 标准库 | 29 模块 (~2.5k 行) |
| EDSL 自修改 | set-code → mutate → query → eval-current + colony:search |
| C FFI | dlopen/dlsym, Int/Float/String/Void |
| 增量编译 | 缓存 + 依赖跟踪 + hot-swap |
| 编译期反射 | P2996 auto_to_json/serialize/deserialize |
| AI Benchmark | 102 任务, Grok **92/102**, DeepSeek 87/102 |
| 执行后端 | 3 个: tree-walk / IR / JIT, 差分测试 198 例 |
| Serve 协议 | closure 检测, 多 session, timeout |

---

## 未完成

### 编译器差异（差分测试 8 个 diff）

| 后端 | 不一致 | 根因 |
|:----|:-------|:-----|
| JIT | `lambda closure` / `length` / `reverse` 返回 0 | 复杂原语 stdlib 函数在 JIT 中未正确调度 |
| IR | `map` / `filter` 返回 `()` | IR 路径中的 stdlib 函数调用不支持对值做 map |
| IR | `file-exists` 打印 primitive 名 | PrimCall 结果格式化问题 |

需要让 IR/JIT 路径正确调用 stdlib 定义的函数（`length`、`reverse`、`map` 等），而不是仅支持 C++ 层注册的原语。

### 等价变异 fuzz

用 EDSL 做**语义保持变换**：重命名变量、交换加法操作数、内联函数 → 输出应与原程序一致。验证编译器在不同输入下的等价性。

### P2 — 语言扩展

| 项目 | 工作量 |
|:-----|:------:|
| Numerical arrays `#(1 2 3)` | 3d |
| 错误诊断（行列号/suggestion） | 2d |
| Serve 并发安全 | 2d |
| FFI Opaque/Struct/回调 | 3d |

### P3 — 项目工具（远用）

二进制输出 / LSP / 包管理 / 自举

---

## 回看

- **2026-05-23**: P0-P3 全关, 102 任务 benchmark, 4600+ fuzz 0 crash, ASan/UBSan 通过, 3 后端差分测试, 7 个编译器 bug 修复
