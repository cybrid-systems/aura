# Aura 演进路线图（ROADMAP）

> **增量式构建，始终保持可运行。**  
> 每个 Phase 结束时，系统都有一个可工作的、经过测试的新版本。

---

## 总览

| Phase | 名称 | 目标 | 预计周期 | Racket | C++26 |
|-------|------|------|----------|--------|-------|
| 0 | 种子 | 最小 Lisp 核心可运行 | 1–2 周 | ✅ 核心 | — |
| 1 | 底座 | 工程级强度 + 增量编译 | 2–4 周 | ✅ 扩展 | 🔧 原型 |
| 2 | 反射 | 运行时环境操作 + flambda | 3–5 周 | ✅ 核心 | 🔧 原型 |
| 3 | 宏 | 卫生宏 + 持久 AST 对象化 | 4–6 周 | ✅ 核心 | 🔧 迁移 |
| 4 | 生产 | LLVM 后端 + 生态闭环 | 持续 | 🟡 维护 | ✅ 主力 |

> **图例**：✅ 主力语言  🔧 开始迁移  🟡 维护模式

---

## Phase 0：种子（Seed）

**目标**：最小的、可运行的 Lisp 核心。这个核心小到 AI 可以完全理解，同时强大到可以表达通用计算。

### 实现清单

| # | 特性 | Racket | 测试 | 说明 |
|---|------|--------|------|------|
| 0.0 | 项目骨架与 #lang 包 | ✅ | ✅ | 包结构、`#lang aura`、raco test 管线 |
| 0.1 | 字面量求值 | ✅ | ✅ | 数字、字符串、布尔 |
| 0.2 | lambda 与函数应用 | ✅ | ✅ | Lisp1 语义，lexical scoping |
| 0.3 | if 条件 | ✅ | ✅ | 完整条件分支 |
| 0.4 | let 绑定 | ✅ | ✅ | 语法糖展开为 lambda |
| 0.5 | quote 与基本数据 | ✅ | ✅ | list, pair, symbol |
| 0.6 | define（Hyperstatic） | ✅ | ✅ | 全局定义，不可 set! 覆盖 |
| 0.7 | letrec | ✅ | ✅ | 相互递归支持 |
| 0.8 | 基本 REPL | ✅ | ✅ | read-eval-print 循环 |

### 交付物
- `#lang aura` Racket 包，`raco test` 管线就绪
- 完整实现 [DESIGN.md §4](../DESIGN.md#4-语言核心phase-0-语义规范) 定义的最小核心语法
- 基本 REPL：`racket -l aura`
- 可运行 `(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 5))`
- 测试覆盖度 > 90%

### AI Agent 能力
- 代码作为数据结构（S 表达式）可被构造和查询
- 能运行简单的递归规划

### 参考
- Ghuloum 第一步到第六步
- LiSP Ch2（基础求值器）
- Racket `#lang` 创建指南

---

## Phase 1：底座（Foundation）

**目标**：让语言具备生产级工程能力，为 Agent 提供可靠的静态分析基础。

### 实现清单

| # | 特性 | Racket | C++26 | 说明 |
|---|------|--------|-------|------|
| 1.0 | 尾调用优化（TCO） | ✅ | 🔧 | 保证递归不爆栈 |
| 1.1 | 完整预处理管道 | ✅ | — | compute-kind, arity 检查 |
| 1.2 | 静态错误检测 | ✅ | — | 未绑定变量、参数数量不匹配 |
| 1.3 | 多名字空间分离 | ✅ | — | 配置/工具/Prompt/AST 空间隔离 |
| 1.4 | 受控赋值与内存函数 | ✅ | — | set! + update/update* |
| 1.5 | Context 对象与 with | ✅ | — | 显式环境上下文传递 |
| 1.6 | 对象系统（优于闭包） | ✅ | — | 基于 record 的简单对象 |
| 1.7 | 增量编译框架 | ✅ | 🔧 | 版本化编译单元 |
| 1.8 | C++26 最小解释器 | — | ✅ | threaded code VM 原型 |
| 1.9 | 性能基准测试 | ✅ | ✅ | 对比 Racket vs C++26 |

### 交付物
- Racket 编译器能处理 1000+ 行 Agent 代码
- C++26 原型解释器能运行 Phase 0 核心
- 性能对比：C++26 版本比纯 Racket 解释器快 20–50 倍
- 增量编译：只重新编译变更的部分

### AI Agent 能力
- 递归 Agent / Tree-of-Thoughts 高效运行
- 工具调用、状态版本化、回溯
- 静态错误提前检测（Agent 修改代码后立即校验）

### 参考
- Ghuloum 第七步到第十步（编译）
- LiSP Ch3（预处理）+ Ch6（指称语义）
- C++26 std::generator + 编译期反射（std::meta / clang plugins）

---

## Phase 2：反射（Reflection）

**目标**：让 Agent 具备"自我认知 + 自我修改"能力。这是"自然生长"的关键转折。

### 实现清单

| # | 特性 | Racket | C++26 | 说明 |
|---|------|--------|-------|------|
| 2.0 | export | ✅ | 🔧 | 将内部绑定暴露给外部环境 |
| 2.1 | eval/b | ✅ | 🔧 | 在指定环境中求值表达式 |
| 2.2 | enrich | ✅ | 🔧 | 在新环境基础上扩展绑定 |
| 2.3 | flambda（FEXPR） | ✅ | 🔧 | 未求值参数 + 当前环境 |
| 2.4 | reflective-lambda | ✅ | 🔧 | 受控反射 lambda |
| 2.5 | import（准静态绑定） | ✅ | 🔧 | 性能友好版 eval/b |
| 2.6 | 显式环境对象 | ✅ | 🔧 | 环境作为第一类值 |
| 2.7 | C++26 反射运行时 | — | ✅ | export/eval/b 的 C++ 实现 |
| 2.8 | 跨语言反射测试 | ✅ | ✅ | Racket ↔ C++26 环境互操作 |

### 交付物
- Agent 能在运行时 export 自己的模块
- flambda 执行自定义求值策略（如：先模拟再实际执行）
- 演示：自修改 Agent——Agent 输出代码、加载、测试、修改循环
- C++26 运行时支持基本反射能力

### AI Agent 能力
- **动态重写自己的推理策略**、规划逻辑、工具集
- 热更新 + 插件系统
- "思考→规划→执行"闭环（类似 o1 的内部推理）

### 参考
- LiSP Ch8（反射与元求值器）
- 3-Lisp（Brian Smith）的反射塔
- Brown 的 FEXPR 研究

---

## Phase 3：宏（Macro）

**目标**：语言学会"语言生长"——通过宏系统，AI Agent 可以自主扩展语法。

### 实现清单

| # | 特性 | Racket | C++26 | 说明 |
|---|------|--------|-------|------|
| 3.0 | 卫生宏系统 | ✅ | 🔧 | 默认卫生，与 Racket 对齐 |
| 3.1 | with-aliases | ✅ | 🔧 | 显式逃生舱 |
| 3.2 | 宏定义宏 | ✅ | — | 宏可以生成宏 |
| 3.3 | 持久 AST 对象化 | ✅ | 🔧 | AST 作为第一类对象 |
| 3.4 | AST 模式匹配 | ✅ | 🔧 | 声明式 AST 查询和变换 |
| 3.5 | Code Walking | ✅ | 🔧 | 宏可遍历任意深度 AST |
| 3.6 | Expansion/Execution 分离 | ✅ | — | 宏展开和求值严格分界 |
| 3.7 | 多层宏展开 | ✅ | — | 宏展开后的结果可以包含宏 |
| 3.8 | C++26 宏运行时 | — | ✅ | AOT 宏展开 + 缓存 |

### 交付物
- 完整的卫生宏系统，支持宏定义宏
- 持久 AST：从解析到编译到运行时全程保留源位置注释
- 演示：Agent 用宏扩展出新 DSL 并立即使用

### AI Agent 能力
- **完全重写接口和语法**：Agent 可定义新的控制结构、DSL
- 动态生成领域特定语言
- 跨 Agent 代码迁移与交换（通过持久 AST 序列化）

### 参考
- LiSP Ch9（宏与对象化）
- Racket `syntax-parse` 文档
- Matthew Flatt 的宏研究

---

## Phase 4：生产（Production）

**目标**：让语言既能"自然生长"，又能无缝替换旧生态。

### 实现清单

| # | 特性 | Racket | C++26 | 说明 |
|---|------|--------|-------|------|
| 4.0 | LLVM IR 后端 | 🟡 | ✅ | 原生代码生成 |
| 4.1 | 完整模块系统 | 🟡 | ✅ | 编译单元 + 模块间分析 |
| 4.2 | 静态类型可选层 | 🟡 | ✅ | 基于编译期反射的可选静态类型 |
| 4.3 | 分布式续延 | — | ✅ | checkpoint/resume |
| 4.4 | 外部工具互操作 | — | ✅ | Python/LLM 调用接口 |
| 4.5 | 安全沙箱 | — | ✅ | 基于 reflective-lambda 的权限 |
| 4.6 | 自举（Bootstrap） | — | ✅ | Racket → Aura 自编译 |
| 4.7 | 持久化 Agent 运行时 | — | ✅ | 状态持久化 + 恢复 |
| 4.8 | 包管理器 | 🟡 | ✅ | 模块分发和版本管理 |

### 交付物
- Aura 自举——编译器用 Aura 自己实现
- LLVM 后端，启动速度接近 native
- Agent 沙箱安全控制
- 完整的包注册表

### AI Agent 能力
- 大规模多 Agent 协作
- 生产级部署
- "用 Aura 重写 Python 工具"的演示

### 参考
- LLVM IR 教程
- Ghuloum 完整的增量编译
- WASM 沙箱设计

---

## 增量构建红线

每个 Phase 结束时的验证标准：

```
Phase 0:  racket -e '(print (quote (hello world)))'  → (hello world)
Phase 1:  Incremental compile 1000-line program  → < 100ms relink
Phase 2:  Agent eval/b-edits its own strategy  →  runtime behavior changes
Phase 3:  Agent defines a DSL macro  →  uses it in next line
Phase 4:  Aura compiles itself  →  bootstrap complete
```

---

## 时间线

```
Week  1-2:  Phase 0 种子
Week  3-6:  Phase 1 底座
Week  7-11: Phase 2 反射
Week 12-17: Phase 3 宏与对象
Week 18+:   Phase 4 生产（持续）
```

每个 Phase 之间留 1 周缓冲用于整合和文档。

---

## 验证方法

**每个 Phase 结束时的演示**：
1. **Self-Evolving Agent Demo**：Agent 自己修改自己代码并继续运行
2. **性能基准**：对比上一个 Phase 的性能改进
3. **测试覆盖度**：> 90%

---

## 文件结构

```
aura/
├── docs/
│   ├── DESIGN.md        # 设计哲学与架构
│   └── ROADMAP.md       # 演进路线图
├── prototypes/
│   └── racket/          # Racket 原型（Phase 0-3 主力）
│       ├── main.rkt
│       ├── env.rkt      # 环境模型
│       ├── eval.rkt     # 求值器
│       ├── syntax.rkt   # 宏展开
│       └── test/        # 测试套件
├── src/                 # C++26 生产实现（Phase 1+）
│   ├── ast/
│   ├── eval/
│   ├── compile/
│   └── vm/
├── test/                # C++26 测试
├── CMakeLists.txt
└── README.md
```
