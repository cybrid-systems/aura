# Aura 路线图

**更新：2026-05-23**

---

## 当前能力

| 维度 | 状态 |
|------|:----:|
| 编译器 | 0 crash fuzz (4600+ cases), 37 维结构化测试, 3 个执行后端 |
| 类型系统 | Sound Gradual Typing, ADT + match 穷尽性, let-poly, blame |
| M4 线性所有权 | move/borrow/drop 编译期 + 运行时检测 |
| 标准库 | 29 模块 (~2.5k 行): list/string/hash/math/JSON/CSV/socket/vector |
| EDSL 自修改 | set-code → mutate → query → eval-current + colony:search |
| C FFI | dlopen/dlsym 调用, Int/Float/String/Void 编组 |
| 增量编译 | 缓存 + 依赖跟踪 + hot-swap |
| 编译期反射 | P2996 auto_to_json/serialize/deserialize |
| AI Benchmark | 102 任务, Grok **92/102 (90.2%)**, DeepSeek 87/102 |
| Fuzz 体系 | 3 套 fuzz, 4600+ 用例, 48 测试维度, 全 0 crash |

---

## 方向 & 优先级

### P1 — 编译器深度加固

| 项目 | 原因 | 工作量 |
|:-----|:-----|:------:|
| ASan/UBSan 编译跑 fuzz | 最高 ROI — 即刻暴露内存 bug | 1h |
| 差分测试（树遍历 vs IR vs JIT） | 三个后端输出一致性问题 | 2d |
| 等价变异 fuzz（EDSL 语义保持变换） | 利用已有 EDSL 能力测编译器 | 2d |
| `( ' car )` vector OOB | 已知 1 个 fuzz 边缘 case | 4h |

### P2 — 语言能力扩展

| 项目 | 原因 | 工作量 |
|:-----|:-----|:------:|
| Numerical arrays | `#(1 2 3)` 语法, 连续内存, 数值计算 | 3d |
| 更好的错误诊断 | 行列号精确定位, suggestion, blame trace 格式化 | 2d |
| Serve 并发 | 多 session 安全, 无竞态 redefine | 2d |
| FFI 扩展 | Opaque 指针, Struct marshal, 回调支持 | 3d |

### P3 — 项目级工具（远用）

| 项目 | 说明 |
|:-----|:------|
| 二进制输出 | LLVM IR → .o → ld → ELF standalone |
| LSP/IDE | 语法高亮, 补全, 内联诊断 |
| 包管理 | registry, 依赖声明, 版本解析 |
| 自举 | 编译器用 Aura 重写 |

---

## 报告回顾

- **2026-05-23 早**：85 任务, 3 P0 bug, 无 fuzz, 14h 工作后
- **2026-05-23 晚**：102 任务, P0-P3 全关, 4600+ fuzz 0 crash, serve closure 协议检测, parser 深度守卫, EDSL 变异 fuzz, 37 维结构化测试

## 相关文档

- [基准测试](benchmark.md) — 102 任务 AI 代码生成评估
- [语言规范](design/aura_language_spec.md) — 语法、类型系统、EDSL
