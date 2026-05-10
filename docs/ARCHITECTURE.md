# Aura — 架构设计

**版本**：v1.0
**对应**：Phase 0 已完成 → Phase 1 工程化进行中
**定位**：本文档描述 Aura 语言的系统架构、模块分解、接口约定和数据流。设计哲学与核心决策见 [DESIGN.md](./DESIGN.md)，演进计划见 [ROADMAP.md](./ROADMAP.md)。

---

## 1. 三层架构总览

```
┌──────────────────────────────────────────────────────────────────────┐
│                        Racket Frontend                               │
│                                                                      │
│  #lang aura + syntax-parse 宏系统                                     │
│  → Homoiconic AST (Trees that Grow 扩展 + 源位置保留)                 │
│  → ABF 序列化输出 (零拷贝 + Delta 增量)                                 │
│                                                                      │
│  Phase 0-3 主力语义验证平台                                           │
└───────────────────────────────────┬──────────────────────────────────┘
                                    │
                   共享内存 / Unix Domain Socket
                                    │
┌───────────────────────────────────▼──────────────────────────────────┐
│                     Compiler Service (C++26)                         │
│                                                                      │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │  AST Layer — Trees that Grow 恢复 & 静态反射                   │  │
│  │  (Parsed → Typed → Located → ...)                              │  │
│  └──────────────────────────┬─────────────────────────────────────┘  │
│                             │ Lowering Pass                         │
│  ┌──────────────────────────▼─────────────────────────────────────┐  │
│  │  AuraIR Layer — 自定义 SSA-like IR                              │  │
│  │  (函数级/子树级增量追踪)                                       │  │
│  └──────────────────────────┬─────────────────────────────────────┘  │
│                             │                                        │
│  ┌──────────────────────────▼─────────────────────────────────────┐  │
│  │  AuraQueryEngine                                              │  │
│  │  Lucene 风格倒排索引 + Aura eDSL (AI 查询/变换/修复入口)      │  │
│  └──────────────────────┬───────────────────┬─────────────────────┘  │
│                         │                   │                        │
│  ┌──────────────────────▼───────────────────▼─────────────────────┐  │
│  │  增量优化 Pass Chain + 热更新引擎                              │  │
│  │  (函数级/子树级无缝替换, 零停机)                              │  │
│  └──────────────────────┬───────────────────┬─────────────────────┘  │
│                         │                   │                        │
│  ┌──────────────────────▼───────────────────▼─────────────────────┐  │
│  │  三层运行时                                                   │  │
│  │  Layer 1: 动态解释器 (快速启动/调试)                          │  │
│  │  Layer 2: LLVM ORC JIT (渐进编译)                             │  │
│  │  Layer 3: AOT C++26 二进制 (最高性能)                         │  │
│  └────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘
```

| 层 | 位置 | 技术栈 | 核心职责 |
|----|------|--------|----------|
| **Racket Frontend** | Racket | #lang + syntax-parse | 宏展开、语义验证、Homoiconic AST 输出 |
| **ABF** | 通信层 | 自定义二进制 | 零拷贝序列化、Delta 增量传输 |
| **AST 层** | C++26 | Trees that Grow + Concepts | 早期处理、静态反射、源位置保留 |
| **AuraIR 层** | C++26 | 自定义 SSA-like IR | 优化核心、代码生成入口 |
| **AuraQueryEngine** | C++26 | 倒排索引 + eDSL | 子树查询、变换、AI 接口 |
| **Compiler Service** | C++26 | 长驻进程 | 编译 + 执行 + 热更新 + 状态 |
| **三层运行时** | C++26 | 解释/JIT/AOT | 执行 + 热更新 + 性能分层 |

---

## 2. 关键设计决策

| 决策点 | 选择 | 理由 |
|--------|------|------|
| 前端语言 | Racket (轻量 #lang) | 最强宏系统 + 真正 homoiconic，AI 原型速度最快 |
| 后端 | C++26 Compiler as a Service | 性能 + 状态管理 + AI 交互 + 热更新 |
| 中间表示 | AST (Trees that Grow) + AuraIR | 早期 AST（结构完整）、后期 IR（优化友好） |
| 查询系统 | AuraQueryEngine (Lucene 风格 + eDSL) | AI 最友好，支持精准子树级增量 |
| AOT 路径 | 生成 C++26 源码（短期）→ LLVM IR（长期） | 快速验证，后期直出 native |
| 增量粒度 | 函数级 + 子树级 | 通过 QueryEngine 实现精准热更新 |
| AI 交互 | AuraQuery eDSL + AI_API | 原生语法 + 直接操作 IR/运行时 |
| 类型系统 | Sound Gradual Typing | 渐进演化，AI 可逐步加强约束 |
| 名字空间 | Lisp1（函数变量同空间） | 更简单，更适合 AI 推理 |
| 作用域 | Hyperstatic（全局定义不可覆盖） | AI 可安全缓存全局绑定信息 |
| 卫生宏 | 默认卫生，with-aliases 逃生舱 | DSL 生成需要受控的标识符捕获 |
| 反射 | 静态反射（编译期）+ 运行时反射（flambda）双轨 | 性能敏感场景 vs 动态演化场景 |

---

## 3. 模块详细设计

### 3.1 Racket Frontend

**模块结构**：

```
lang/
├── reader.rkt        # #lang aura 读取器 (S 表达式入口)
├── expander.rkt      # 宏展开器 + 核心求值器
├── compiler.rkt      # ABF 序列化输出
└── private/
    ├── core.rkt      # 最小核心原语 (Phase 0 语法规范)
    ├── macro.rkt     # 卫生宏系统 (Phase 3 起)
    └── types.rkt     # 可选类型标注 (Phase 2 起)
```

**职责**：
- 将人类/AI 的 S 表达式源码解析为 Homoiconic AST
- 通过 `syntax-parse` 执行宏展开（卫生宏、with-aliases 逃生舱）
- 将展开后的 AST 序列化为 ABF 二进制格式，通过共享内存传输到 C++26 Compiler Service

**与 Compiler Service 的关系**：

```
┌──────────────────────┐       ABF + Delta        ┌────────────────────────┐
│  Racket Frontend     │ ────── 共享内存 ──────→   │  Compiler Service      │
│  (#lang aura)        │ ←──── 结果/错误 ─────── │  (C++26 长驻进程)      │
└──────────────────────┘     Unix Domain Socket   └────────────────────────┘
```

---

### 3.2 ABF — Alien Binary Format

**设计目标**：零拷贝 + Delta 增量传输 + 跨语言边界的极高性能序列化。

**格式概览**（完整规范见 [SERIALIZATION.md](./SERIALIZATION.md)）：

```
┌───────────────────────────────────┐
│  Header (64 bytes)               │
│  - Magic: "ABF1"                 │
│  - Version, Flags, NodeCount    │
│  - StringTableOffset             │
│  - DeltaBaseVersion              │
├───────────────────────────────────┤
│  Node Table (每个节点 32 bytes)   │
│  - node_id, parent_id           │
│  - kind, flags                  │
│  - payload_offset, payload_size │
│  - source_pos (file:line:col)   │
├───────────────────────────────────┤
│  Payload Region (节点负载)        │
├───────────────────────────────────┤
│  String Table (去重字符串池)      │
├───────────────────────────────────┤
│  Delta Patch (可选)              │
│  - Base version ref              │
│  - Changed node IDs              │
│  - New payload segments          │
└───────────────────────────────────┘
```

**关键特性**：
- **零拷贝**：Node Table 固定大小条目，可直接 mmap 读取
- **Delta 增量**：只传输变化的子树，大幅减少通信量
- **源位置保留**：每个节点携带精确的 `file:line:col`
- **预分配内存池**：避免序列化时的内存碎片和 GC 压力

---

### 3.3 AST 层 (Trees that Grow)

**技术选型**：C++26 `std::variant` + Concepts + Trees that Grow 模式

**核心设计**（完整实现见 [SERIALIZATION.md](./SERIALIZATION.md)）：

```cpp
// 不同阶段携带不同扩展数据
struct ParsedPhase  { using extension_type = std::monostate;    static constexpr uint32_t id = 0; };
struct TypedPhase   { using extension_type = TypeInfo;          static constexpr uint32_t id = 1; };
struct LocatedPhase { using extension_type = SourceLocation;    static constexpr uint32_t id = 2; };

// 每个节点类型独立可扩展
template <Extension E>
struct Expr : std::variant<
    LiteralInt<E>, Variable<E>, Call<E>,
    IfExpr<E>, Lambda<E>, ExtendedNode<E>
> {};
```

| 阶段 | Extension 携带的信息 |
|------|---------------------|
| Parsed | 裸语法结构 |
| Expanded | 宏展开绑定信息、作用域链 |
| Typed | 类型标注、类型约束 |
| Lowered | AuraIR 映射信息 |
| Optimized | 优化标记、内联决策 |
| CodeGen | 代码生成元数据 |

---

### 3.4 AuraIR 层

**设计目标**：
- 足够低级：支持精确优化 Pass（内联、循环变换、死代码消除）
- 保持可追踪：每个 IR 指令携带源 AST 节点 ID
- 支持热更新：函数级替换

**最小 IR 指令集**（Phase 1）：

```
Nop, Const, Arg, Local           // 数据
Add, Sub, Mul, Div,              // 算术
Eq, Lt, Gt, And, Or, Not,        // 逻辑
Branch, Jump, Call, Return,      // 控制流
MakeClosure, Capture, Apply      // 闭包
```

**基本块结构**：

```cpp
struct BasicBlock {
    uint32_t id;
    std::vector<IRInstruction> instructions;
    std::vector<uint32_t> predecessors, successors;
    uint32_t source_ast_node_id;
};

struct IRFunction {
    std::string name;
    std::vector<BasicBlock> blocks;
    uint32_t entry_block_id;
    std::vector<Symbol> free_variables;
};
```

---

### 3.5 AuraQueryEngine + eDSL

**架构**：

```
AuraQueryEngine
├── Index Layer
│   ├── AST Index         — 节点类型/模式/子树
│   ├── IR Index          — IR 指令/基本块
│   ├── Def-Use Index     — 定义-使用链
│   └── Source Index      — 源位置 → 节点
├── Query Layer
│   ├── AuraQuery Parser  — eDSL 解析
│   ├── Query Executor    — 索引查找 + 排序
│   └── Pattern Matcher   — 树结构模式匹配
└── Mutation Layer
    ├── Patch Generator    — 查询结果 → 增量补丁
    └── Hot Swap Scheduler — 调度热更新
```

**AuraQuery eDSL 示例**：

```lisp
;; 查找所有递归调用
(aura-query
  (select (node :kind 'Lambda)
          (where (exists (child :kind 'Call
                               (where (target :name 'self)))))))

;; 查找未使用的函数定义
(aura-query
  (select (node :kind 'Define)
          (where (= (use-count :node) 0))))

;; 给函数调用添加日志
(aura-transform
  (select (node :kind 'Call) (where (not (target :name 'log))))
  (transform (wrap-with 'with-logging)))

;; 替换特定子树
(aura-patch
  :target (select (node :id 42))
  :replacement '(lambda (x) (* x x)))
```

---

### 3.6 Compiler as a Service

**服务架构**：

```
Compiler Service
├── IPC Layer
│   ├── Unix Domain Socket (本地进程通信)
│   ├── 共享内存通道 (ABF 传输)
│   └── RESTful API (远程管理)
├── Compilation Pipeline
│   ├── AST Deserializer
│   ├── Lowering Pass
│   ├── Optimizer (增量 Pass Chain)
│   └── Code Generator
├── Runtime Layer
│   ├── Triple Runtime Manager
│   ├── Hot Swap Engine
│   └── State Persistence
├── AI Interface
│   ├── AuraQuery Executor
│   ├── API Gateway
│   └── Event Subscription
└── Management
    ├── Compilation Cache
    ├── Module Registry
    └── Performance Telemetry
```

**核心接口草稿**：

```cpp
class AuraCompilerService {
public:
    // 编译
    CompileResult compile(ABFBuffer ast_buffer);
    CompileResult incremental_compile(ABFDelta delta);
    
    // 执行
    EvalResult evaluate(const std::string& expr, EvalMode mode);
    EvalResult evaluate_in_env(const std::string& expr, EnvironmentId env);
    
    // 查询
    QueryResult query(const AuraQuery& q);
    QueryResult query_source_line(uint32_t line, const std::string& file);
    
    // 热更新
    HotSwapResult hot_swap(FunctionId target, ABFBuffer new_impl);
    HotSwapResult hot_swap_subtree(ASTNodeId target, ABFBuffer replacement);
    
    // AI 接口
    PatchResult apply_patch(ASTPatch patch);
    QueryResult ai_query(aura_sexpr query_s_expr);
    
    // 持久化
    void save_state(const std::string& path);
    void load_state(const std::string& path);
};
```

---

### 3.7 三层运行时

| 层级 | 名称 | 启动 | 执行速度 | 热更新 | 场景 |
|------|------|------|----------|--------|------|
| Layer 1 | 动态解释器 | 即时 | 最慢 | 完全支持 | 开发、REPL、AI 快速迭代 |
| Layer 2 | LLVM ORC JIT | ~ms | 中等 | 部分支持 | 日常运行、中等负载 |
| Layer 3 | 静态 AOT (C++26) | 编译时 | 最快 | 不支持 | 生产发布、性能关键路径 |

**热更新引擎策略**：
- 函数在解释器中运行 → 直接替换下次调用
- 函数被 JIT 编译 → 使 JIT 代码无效，下次调用重新编译
- 函数被 AOT 编译 → 记录热补丁表，每次调用检查

---

## 4. 主要数据流

### 4.1 正常执行流

```
Racket 源码 → 宏展开 → Homoiconic AST → ABF 序列化
→ 共享内存传输 → Compiler Service 反序列化
→ AuraIR → 增量优化 → 三层运行时
```

### 4.2 AI 修复 / 增量编译流 ← 核心 AI-native 能力

```
错误/需求发生 → QueryEngine 查询目标子树
→ AI 用 AuraQuery eDSL 提交修复补丁
→ 直接修改 AuraIR (或 AST)
→ 增量优化 Pass → 热更新引擎 → 运行时无缝替换
```

### 4.3 AOT 发布流

```
AuraIR → CodeGen → C++26 源码生成
→ clang++ -ffreestanding -O3 → 裸机/独立二进制
```

---

## 5. 语言核心 (Phase 0 语义规范)

```bnf
<expr> ::= <literal>                          ;; 字面量 (数字、字符串、布尔)
         | <symbol>                            ;; 变量引用
         | (lambda (<arg>...) <expr>)          ;; 函数
         | (<expr> <expr>...)                  ;; 函数应用
         | (if <expr> <expr> <expr>)           ;; 条件
         | (let ((<name> <expr>)...) <expr>)   ;; 局部绑定
         | (letrec ((<name> <expr>)...) <expr>) ;; 递归绑定
         | (quote <datum>)                     ;; 字面数据
         | (define <name> <expr>)              ;; 全局定义 (Hyperstatic)
```

这是一个完整、自洽的最小 Lisp 核心——能表达一切计算，同时小到 AI 可完全理解。

---

## 6. 与现有项目的设计对比

| 维度 | Aura | Rust Analyzer | Clangd | Julia |
|------|------|---------------|--------|-------|
| 语言设计目标 | AI-native 语言 | 现有语言 LSP | 现有语言 LSP | 科学计算 |
| AST 形式 | Homoiconic | 传统 AST | 传统 AST | Homoiconic 风格 |
| 增量粒度 | 子树级 (精准) | 文件级 | 文件级 | 函数级 |
| 查询系统 | AuraQuery eDSL | 无 | 无 | 无 |
| AI 接口 | 原生 (Homoiconic + eDSL) | 无 | 无 | 无 |
| 运行时 | 三层 (解释/JIT/AOT) | 非运行时 | 非运行时 | 单层 JIT |
| 热更新 | 函数级 + 子树级 | 不支持 | 不支持 | 包级重载 |
| 通信 | ABF (零拷贝 + Delta) | JSON-RPC | JSON-RPC | 内部 |
| 服务模式 | Compiler as a Service | LSP Server | LSP Server | 进程内 |

---

## 7. 与 ai-programming-language-design 仓库的关系

`aura/` 是具体实现仓库，`ai-programming-language-design/` 是设计研究仓库。

| 内容 | ai-programming-language-design | aura |
|------|-------------------------------|------|
| 设计哲学 | DESIGN_PHILOSOPHY.md | 精炼版 → [DESIGN.md](./DESIGN.md) |
| 架构设计 | 多篇分阶段 | 整合版 → 本文档 |
| Racket 学习 | racket/day-01 ~ day-14 | 已内化为 #lang aura 实现 |
| C++26 标准跟踪 | cpp26/ | 已内化为 Compiler Service 实现 |
| 内核分析 | code-learn/linux/ | 提取语义约束供类型/IR 设计 |
| 实现 | 无 | 有 (Racket #lang + C++26 服务) |

---

> **The Narrow Gate.**
> Aura 不兼容旧生态、不优化给人读。它的唯一目标是：在给定算力下，让 AI 最大程度地理解、验证、重写并统一整个软件栈。
