# M3: 反射 (Reflection) 规划方案

**版本**：v0.1（草案）
**依赖**：M1 IR 管线 ✅ + M2 AuraQuery 引擎 ✅ + GCC 16.1 P2996
**状态**：规划阶段

---

## 1. M3 定位

M1 把代码变成可执行的数据（AST/IR），M2 让 AI 能查询和变换这些数据。
M3 要做的：**让代码能"看见自己"并"修改自己"**——编译期反射（P2996）和运行时反射。

```
M1: Code → AST → IR → Execute
    (编译管线)

M2: Query → Match → Transform → Fix
    (查询变换引擎)

M3: Reflect → Introspect → Generate → Mutate
    (反射元编程)
```

---

## 2. 三层反射架构

```
M3 Reflection Engine
│
├── Layer 1: Compile-time Reflection (P2996)
│   ├── Struct → JSON Schema (auto-generate protocol)
│   ├── IRModule → Serialization (auto to_json/from_json)
│   ├── AST → FlatBuffer schema
│   └── Diagnostic → structured error format
│
├── Layer 2: Runtime Reflection (flambda)
│   ├── Closure introspection
│   ├── Custom evaluation strategies
│   └── Environment inspection
│
└── Layer 3: Metaprogramming (Macros)
    ├── Hygienic macro expansion
    ├── Compile-time AST transformation
    └── AI-generated code verification
```

---

## 3. Layer 1: 编译期反射 (P2996)

### 3.1 现状

GCC 16.1 已完整实现 P2996R13 Reflection，支持：
- `^T` — 反射运算符
- `std::meta::members_of(T)` — 成员列表
- `std::meta::name_of(m)` — 成员名
- `std::meta::type_of(m)` — 成员类型
- `std::meta::is_data_member(m)` — 判断数据成员
- `std::meta::reflect_constant(val)` — 值反射
- Expansion statements (P1306) — 编译期循环

### 3.2 自动序列化

```cpp
// 目标: 一行代码生成任意 struct 的 to_json
template <typename T>
std::string auto_to_json(const T& obj) {
    std::string json = "{";
    template for (auto m : data_members_of<T>()) {
        json += std::format("\"{}\":{}, ",
            std::meta::name_of(m), obj.[:m:]);
    }
    json += "}";
    return json;
}

// 使用:
Diagnostic d{...};
auto json = auto_to_json(d);
// → {"kind":3,"msg":"unbound variable: x","node_id":42}
```

### 3.3 映射到 Aura 类型

| 类型 | 反射生成 | 当前方案 | 迁移时机 |
|------|----------|----------|----------|
| `Diagnostic` | `auto_to_json()` | 手写 `std::format` | GCC 16.1 可用后 |
| `IRInstruction` | IR 指令序列化 | 手写 | M3.2 |
| `IRFunction` | 函数内省 | — | M3.3 |
| `FlatAST` (SoA) | 节点遍历 | `NodeView` + 索引 | M3.4 |
| `Patch` | 补丁序列化 | 手写 | M3.1 |

### 3.4 验证 Demo

`tests/reflect_json_demo.cpp` 已写好，GCC 16.1 下编译运行：

```bash
g++ -std=c++26 -freflection tests/reflect_json_demo.cpp -o build/reflect_demo
./reflect_demo
# → 编译期打印 struct members
# → 编译期生成 JSON Schema
# → 运行时序列化 Diagnostic → JSON
```

---

## 4. Layer 2: 运行时反射 (flambda)

### 4.1 闭包内省

当前 IR 闭包在运行时不可见（只有 `runtime_closures_` map）。
M3 应暴露闭包信息给 Agent：

```cpp
struct ClosureInfo {
    uint32_t func_id;
    std::string_view name;
    std::vector<std::string> param_names;
    std::size_t env_size;
    // 运行时自省:
    std::vector<int64_t> capture_values;
};

// Agent 可以查询运行时闭包:
ClosureInfo info = runtime.inspect_closure(closure_id);
```

### 4.2 自定义求值策略 (flambda)

借鉴 flambda（OCaml 的优化中间表示），支持：

```cpp
// 自定义求值器（Agent 可注入）
struct EvalStrategy {
    bool enable_inlining = true;
    bool enable_specialization = false;
    int max_unroll = 3;
};

// 按函数粒度配置
runtime.set_strategy("fact", {.enable_specialization = true});
```

### 4.3 环境检视

```cpp
// 运行时查看变量绑定
auto val = runtime.lookup_env("x");
auto cells = runtime.dump_cells();    // // letrec cells
auto closures = runtime.dump_closures(); // 闭包表
```

---

## 5. Layer 3: 元编程 (Macros)

### 5.1 卫生宏系统

基于 C++26 反射 + Aura 的 homoiconic AST：

```lisp
;; Aura 宏定义
(defmacro (twice expr)
  `(+ ,expr ,expr))

;; 宏展开（编译期）
(twice 5) → (+ 5 5) → 10
```

### 5.2 编译期 AST 变换

```cpp
// 编译期: 用 P2996 检查 AST 结构
consteval void validate_ast(const FlatAST& ast) {
    for (auto id : ast) {
        auto v = ast.get(id);
        if (v.tag == NodeTag::Call && v.children.size() == 1)
            throw std::meta::exception("zero-arg call");
    }
}
```

### 5.3 AI 生成代码验证

AI Agent 生成代码 → 反射验证 → 编译 → 执行：

```
Agent: (let ((x "hello")) (+ x 1))
  ↓ P2996 反射检查类型
Detect: (+ x 1) where x is string, not int
  ↓ 自动修复
Agent: (string-append x " world")
  ↓ 编译通过
Result: "hello world"
```

---

## 6. 与现有组件的集成

```
M3 组件                       M1/M2 使用方式
────────────────────────────────────────────────────
P2996 auto_to_json            --serve JSON 协议取代手写
反射序列化                    ABF 协议生成/验证
闭包内省                      --serve 调试输出
flambda 策略                  eval_ir() 可选参数
宏展开                        --macro CLI 模式
编译期验证                    PassManager 中注册
```

---

## 7. 实现路线图

### Phase 1: P2996 基础 (GCC 16.1 就绪后)

```
Day 1:
  ├── GCC 16.1 升级验证
  ├── reflect_json_demo 编译通过
  └── auto_to_json<Diagnostic> → 替换 --serve JSON 手写

Day 2:
  ├── auto_to_json<IRInstruction> → IR 序列化
  └── auto_to_json<ArityDiagnostic> → 诊断协议

Day 3:
  ├── Expansion statements: 编译期循环
  └── JSON Schema 编译期生成 → 协议文档
```

### Phase 2: 运行时反射

```
Day 4:
  ├── ClosureInfo + inspect_closure()
  ├── eval() 添加策略参数
  └── --inspect CLI 模式

Day 5:
  ├── Environment dump (--env)
  ├── Cell heap inspection
  └── tests + docs
```

### Phase 3: 宏系统

```
Day 6:
  ├── defmacro 解析器
  ├── Hygienic macro expansion
  └── --macro CLI 模式

Day 7:
  ├── 编译期 AST 验证 (P2996)
  ├── AI 生成代码自动检查
  └── 集成测试
```

---

## 8. 开放问题

1. **GCC 16.1 可用性**：Ubuntu 26.04 官方仓库尚未打包 16.1（当前 20260322 snapshot）。
   解决方案：等待更新，或从源码编译 GCC 16.1。

2. **Performance with reflection**：编译期反射在大量 struct 上会影响编译速度。
   预计只在 Diagnostic/IR 指令层面使用，非热点路径，影响可控。

3. **Macro system scope**：卫生宏系统是否需要完整的 syntax-parse 风格，还是
   从简（模板替换）？先做模板替换，后续再完善。

4. **与 Racket #lang 前端的关系**：Racket 前端已有宏系统。C++ 端的宏系统
   是替代还是补充？定位为"IR-level macro"（编译期 FlatAST 变换），
   与 Racket 的 "source-level macro"（文本/语法展开）互补。

---

## 9. 与现有设计文档的关系

| 文档 | 关系 |
|------|------|
| `aura_architecture.md` | §3.3 Trees that Grow 的 Phase 扩展可反射化 |
| `aura_serialization.md` | 反射生成 ABF v2 序列化代码 |
| `aura_query_engine.md` | M3 宏系统作为 M2 QueryEngine 的上层 |
| `aura_cpp26_guide.md` | P2996 反射编码规范 |
| `aura_ast_dod.md` | SoA FlatAST 的反射遍历 |

---

> 本文档是 M3 起始设计，版本 v0.1。GCC 16.1 就绪后立即启动 Phase 1。
