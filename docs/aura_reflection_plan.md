# Aura — 反射实现方案

**版本**：v1.0
**依赖**：GCC 16.1 P2996 (`__cpp_impl_reflection = 202603`) ✅

---

## 1. 现状评估

```
已实现                     未实现
──────────────────────────  ──────────────────────────
auto_to_json<T>()          ABF 序列化/反序列化自动生成
JSON Schema 生成器           IR opcode dispatch 表生成
ClosureSnapshot              Pass 自动注册/生成
EvalStrategy                宏系统 (defmacro)
consteval kNodeMeta 表      编译期 AST 验证
P1306 expansion demo        反射驱动的 patch 验证
NodeTag enum (12种)          AI 生成代码的结构化检查
```

**核心缺口**：P2996 反射目前只用于**独立工具**（`aura-reflect`、`aura-schema`），
没有深入编译器核心管线。ABF 序列化、IR dispatch、Pass 注册都是手写的。

---

## 2. 可反射的编译器组件

```
编译器组件                  反射策略                   当前实现
──────────────────────────  ───────────────────      ─────────
AST NodeTag 枚举            编译期 tag 枚举遍历        手写
ABF 序列化                  NodeTag → 自动生成       手写
ABF 反序列化                NodeTag → switch 生成    手写
IR opcode dispatch          IROpcode → 表驱动       手写 switch
Pass 注册                   Pass 类型 → 自动注册     concepts fold
kNodeMeta                   constexpr 表            手写维护
Patch 验证                  反射检查 node 兼容性     手写
AI 代码验证                 P2996 type trait 检查    未实现
```

---

## 3. 实施方案

### Phase A: 自动化 NodeTag → ABF（3 天）

**目标**：用 P2996 反射自动生成 ABF 序列化/反序列化代码，消除手写 switch。

#### A1: TagDispatch 表（1 天）

利用 `kNodeMeta` constexpr 表和 `meta_of(tag)` 反射，自动生成 `read_node` 的 dispatch：

```cpp
// 当前（手写 switch）：
switch (tag) {
case 0x01: return read_literal_int(r);
case 0x02: return read_variable(r);
// ... 每个新节点都要加一行

// 目标（反射驱动）：
using ReadFn = ast::Expr*(*)(Reader&);
constexpr auto& readers = []() {
    std::array<ReadFn, 12> tbl{};
    tbl[tag_index(LiteralInt)] = read_literal_int;
    tbl[tag_index(Variable)]   = read_variable;
    // ...
    return tbl;
}();
return readers[tag_index(tag)](r);
```

**关键**：`tag_index(t)` 编译期将 NodeTag 映射到 0..N，而非直接使用 enum value（enum value 有间隙 0x01-0x0C，直接用会浪费表空间）。

#### A2: 反射生成 `write_node`（1 天）

```cpp
// 目标：模板函数为任意 struct 生成 ABF 序列化
template <typename T>
void abf_serialize(Buffer& buf, const T& obj) {
    // 用 P2996 反射遍历成员
    template for (constexpr auto m : data_members_of(^^T)) {
        // 根据成员类型 emit 对应 tag + 值
        using MT = typename[:type_of(m):];
        if constexpr (is_integral<MT>) {
            put_varint(buf, obj.[:m:]);
        } else if constexpr (is_same<MT, string>) {
            put_string(buf, obj.[:m:]);
        }
    }
}
```

**限制**：目前 GCC 16.1 的 `[:m:]` splice 只能在 `template for` 内对同类型成员工作。
对于异构成员需要 `is_same` 类型 dispatch（已验证可行）。

#### A3: tag ↔ reader 映射自动化（1 天）

```cpp
// 为每种节点类型注册 reader
template <NodeTag tag>
struct ReaderFor;

template <> struct ReaderFor<NodeTag::LiteralInt> {
    static constexpr auto tag = NodeTag::LiteralInt;
    static Expr* read(Reader& r) { return read_literal_int(r); }
};

// 用 constexpr 数组收集所有 reader
constexpr auto readers = std::apply([](auto... ts) {
    return std::array{ ReaderFor<ts>::read... };
}, all_node_tags());
```

**优势**：新增节点类型时，只需加一个 `ReaderFor` 特化，dispatch 表自动更新。

---

### Phase B: IR Opcode 反射（2 天）

#### B1: IROpcode → 字符串映射

```cpp
// 当前（手写 switch）：
constexpr const char* opcode_name(IROpcode op) {
    switch (op) {
    case IROpcode::Nop: return "Nop";
    // 24 种 opcode，每个一行
    }
}

// 目标（P2996 反射）：
consteval std::string_view opcode_name(IROpcode op) {
    // 反射枚举值 → 标识符名
    for (constexpr auto e : enumerators_of(^^IROpcode)) {
        if (extract<IROpcode>(e) == op)
            return identifier_of(e);
    }
    return "<unknown>";
}
```

**效果**：`opcode_name(Add)` → `"Add"`，零手写，加新 opcode 自动同步。

#### B2: 反射驱动的 Pass 调度

```cpp
// 当前（手动 fold）：
run_pipeline<ComputeKindWrap, ArityWrap, ConstantFoldingWrap>(ir_mod);

// 目标（反射注册）：
// 在 pass 定义中用 consteval 注册
struct ConstantFoldingWrap {
    static consteval auto meta() {
        return PassMeta{.name = "const-fold", .run_after = {"compute-kind"}};
    }
};

// 调度器读取编译期注册表，自动排序
```

---

### Phase C: ABF/Racket 端代码生成（3 天）

#### C1: NodeMeta → Racket 代码

从 `kNodeMeta` 编译期生成：

```racket
;; 自动生成的 ABF tag 定义
(define TAG-LITERAL-INT #x01)
(define TAG-VARIABLE    #x02)
;; ... C++ NodeMeta 表驱动
```

#### C2: 双端标签一致性校验

```cpp
// 编译期验证 C++ tag == Racket tag
consteval void verify_abf_tags() {
    // 读取二进制资源中的 racket tag 表
    // 对比 kNodeMeta 的 tag 值
    static_assert(tag_code<LiteralInt>() == 0x01);
    static_assert(tag_code<Variable>() == 0x02);
    // 用 P2996 反射枚举所有 tag 值
}
```

---

### Phase D: 编译期 AST 验证（2 天）

#### D1: Patch 前置检查

```cpp
// 在应用 patch 之前，用 P2996 检查 node_id 合法性
consteval void validate_patch(const FlatAST& ast, const Patch& p) {
    auto node = ast.get(p.node_id);
    // 检查 patch 的字段是否与该节点类型兼容
    contract_assert(is_valid_tag(node.tag));
    contract_assert(p.field_index < meta_of(node.tag).fields_count);
}
```

#### D2: AI 生成代码检查

```cpp
template <typename Fn>
consteval auto check_fn_safety(Fn fn) {
    // 反射检查函数体内是否有 unsafe 操作
    // 如：set! 未绑定变量、预期外的副作用
}
```

---

## 4. 优先级与依赖

```
Phase       依赖              预估    可并行?
─────────────────────────────────────────────────
A1 dispatch 表  kNodeMeta 已有    1 天    Yes
A2 反射序列化    P1306 (已验证)    1 天    Yes (与 A1 并行)
A3 reader 注册  C++20 concept      1 天    No (依赖 A1)
B1 opcode 名称  P2996 enum 反射    1 天    Yes (与 A 并行)
B2 pass 调度    C++20 concept      1 天    No
C1 Racket 生成  kNodeMeta          1 天    Yes
C2 标签校验     A1 完成后          2 天    No
D1 patch 验证   A3 完成后          1 天    No
D2 AI 检查      P2996 (已验证)     1 天    Yes
```

**建议顺序**：

```
Week 1:  A1 + B1 + C1 (并行，互不依赖)
Week 2:  A2 + A3 + C2
Week 3:  B2 + D1 + D2
```

---

## 5. 已知限制

| 限制 | 影响 | 缓解方案 |
|------|------|----------|
| `^^T` 不能在 `template for` 内部对模板参数 T 使用 | A2 的泛型序列化受限 | 手动为每个节点类型特化 serializor |
| `[:m:]` splice 要求 constexpr m | 异构成员 dispatch 需要 is_same 链 | 已验证可行 |
| P1306 `template for` 不能检测"最后一个元素" | schema 生成有尾逗号 | 用 fixup 后处理 |
| GCC 模块 + -freflection 不兼容 | 反射代码必须独立于模块系统 | 保持 `reflect/` 头文件方式 |
| `std::meta::members_of` 需要构造函数 exprs | constexpr string 操作受限 | 用 char buffer 绕过 SSO 问题 |
| `nonstatic_data_members_of` 返回 `vector<info>` | 不能 constexpr 存储 | 用 `member_count<T>()` 做数组大小 |
| `template_of(info)` 抛出异常 | std::array 检测困难 | 用 display_string_of 替代 |

---

## 6. 当前可用的反射工具

```
reflect/reflect.hh          — auto_to_json<T>() 通用序列化
reflect/reflect_schema.hh   — 编译期 JSON Schema 生成
tests/reflect_json_demo.cpp — P2996 功能演示
tools/aura-reflect.cpp      — 独立反射工具 (--expansion demo)
tools/aura-schema.cpp       — 独立 schema 生成器
```

---

## 7. 下一步建议

基于现状评估，**推荐先做 B1**（IROpcode 枚举反射）。

理由：
- 独立于模块系统（头文件方式 `reflect/opcode_reflect.h`）
- 纯 P2996 反射，零其它依赖
- 24 种 IROpcode 的 `opcode_name()` 自动生成，消除手写 switch
- 1 天可完成
- 演示价值高：`aura-reflect --opcodes` 直接输出所有 opcode 信息

A1（dispatch 表）建议第二周做，因为它需要深入编译器核心（`abf_deserializer`），
而 B1 是完全独立的。

---

> 路线图上的 M3 Phase 3b（宏系统）放在这些反射基础设施后面，因为宏系统需要
> 稳定可靠的 node 序列化和编译期验证——这正是 Phase A-D 要交付的。
