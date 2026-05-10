# Aura — ABF v2 序列化协议设计

**版本**：v2.0
**定位**：本文档描述 Aura 的 AST 序列化协议——融合 "Trees that Grow" 模式 + C++26 Concepts + 自定义零拷贝二进制格式。

---

## 1. 设计目标

| 目标 | 优先级 | 实现方式 |
|------|--------|----------|
| 零拷贝反序列化 | P0 | ExtensionLength + `std::span` 切片 |
| 向前/向后兼容 | P0 | ExtensionID + 长度前缀，老版本可跳过未知扩展 |
| 类型安全 | P0 | C++26 Concepts + `std::variant` 封闭变体 |
| AST 可生长 | P0 | template `<Extension E>`，每个阶段新增字段 |
| 反射驱动 | P1 | P2996 `std::meta` 自动序列化 extension 字段 |
| 无外部依赖 | P1 | 纯 C++26，无 protobuf / Cap'n Proto |
| Delta 增量传输 | P2 | 版本化 + 仅传输变更子树 |

---

## 2. "Trees that Grow" 核心思想（简述）

论文（Peyton Jones 等，Microsoft Research 2016）解决的核心问题：

> 传统 AST 一旦定义，后续很难安全地添加新字段（类型注解、位置信息、自定义元数据）或新增节点类型，否则会导致大量废弃字段或代码重复。

**解决方案**：
- 给 AST 增加一个**类型参数** `ξ`（Extension Descriptor / Phase）
- 每个构造器额外携带一个字段 `X ξ`，其类型由 `ξ` 决定
- 不同编译阶段使用不同的 `ξ`（如 `Parsed`、`Typed`、`Optimized`），实现 AST "生长"

**在 Aura 中的对应**：
- 好处：类型安全、无代码重复、支持渐进式演化（完美匹配 Ghuloum 增量开发）
- 扩展：不仅仅是添加字段，我们通过 ABF 二进制格式将这种生长扩展到**序列化层**

---

## 3. C++26 实现：用 Concepts 模拟 Type Families

### 3.1 核心概念定义

```cpp
// aura/ast/concepts.hpp
namespace aura::ast {

template <typename T>
concept Extension = requires {
    typename T::extension_type;          // 每个 Phase 的扩展类型
    { T::id } -> std::convertible_to<uint32_t>;  // 用于序列化
};

// 不同阶段的 Extension 定义
struct ParsedPhase {
    using extension_type = std::monostate;        // 解析阶段无额外数据
    static constexpr uint32_t id = 0;
};

struct TypedPhase {
    using extension_type = TypeInfo;             // 类型检查后增加类型信息
    static constexpr uint32_t id = 1;
};

struct LocatedPhase {
    using extension_type = SourceLocation;       // 增加源代码位置
    static constexpr uint32_t id = 2;
};

} // namespace aura::ast
```

### 3.2 可生长 AST 定义

**设计原则**：每个构造器（节点类型）独立携带自己的扩展数据，而非整个 AST 共用同一个扩展类型。

```cpp
// aura/ast/expr.hpp
namespace aura::ast {

template <Extension E>
struct Expr : std::variant<
    LiteralInt<E>,
    Variable<E>,
    Call<E>,
    IfExpr<E>,
    Lambda<E>,
    ExtendedNode<E>               // ← 预留：未知 Tag 的兜底节点
> {
    using variant::variant;
};

// 每个节点类型独立扩展（每个都有自己的 XExtension）
template <Extension E>
struct LiteralInt {
    int64_t value;
    [[no_unique_address]] typename E::literal_int_ext ext;  // 节点级扩展
};

template <Extension E>
struct Variable {
    Symbol name;
    [[no_unique_address]] typename E::variable_ext ext;
};

template <Extension E>
struct Call {
    ExprPtr<E> func;
    std::vector<ExprPtr<E>> args;
    [[no_unique_address]] typename E::call_ext ext;
};

template <Extension E>
struct IfExpr {
    ExprPtr<E> cond;
    ExprPtr<E> then_branch;
    ExprPtr<E> else_branch;
    [[no_unique_address]] typename E::if_ext ext;
};

template <Extension E>
struct Lambda {
    std::vector<Symbol> params;
    ExprPtr<E> body;
    [[no_unique_address]] typename E::lambda_ext ext;
};

// 兜底：未知 Tag 的节点，保留原始 blob
template <Extension E>
struct ExtendedNode {
    uint32_t original_tag;
    std::span<const std::byte> raw_data;           // 零拷贝引用
    [[no_unique_address]] typename E::ext_node_ext ext;
};

// 智能指针：为 Trees that Grow 语境提供，支持树结构
template <Extension E>
using ExprPtr = std::unique_ptr<Expr<E>>;

} // namespace aura::ast
```

### 3.3 阶段扩展定义示例

```cpp
// aura/ast/extensions.hpp
namespace aura::ast::ext {

// ===== ParsedPhase：裸语法结构 =====
template <>
struct ParsedPhase::literal_int_ext {  // 默认扩展：无额外数据
    static constexpr bool present = false;
};
// ... 其他节点类型的 Parsed 扩展类似，均为 monostate

// ===== TypedPhase：类型信息 =====
template <>
struct TypedPhase::literal_int_ext {
    static constexpr bool present = true;
    int64_t min_value, max_value;      // 字面量范围约束
};

template <>
struct TypedPhase::call_ext {
    static constexpr bool present = true;
    std::vector<TypeInfo> arg_types;   // 参数类型
    TypeInfo return_type;              // 返回类型
    size_t resolution_id;              // 函数重载决议 ID
};

template <>
struct TypedPhase::lambda_ext {
    static constexpr bool present = true;
    std::vector<TypeInfo> param_types;
    TypeInfo return_type;
    std::vector<Symbol> captured_vars; // 闭包捕获集合
};

// ===== LocatedPhase：源位置 =====
template <>
struct LocatedPhase::literal_int_ext {
    static constexpr bool present = true;
    SourceLocation loc;
};
// ... 其他类似

} // namespace aura::ast::ext
```

### 3.4 使用示例

```cpp
// 类型检查阶段可以安全访问扩展数据
void process_function_call(const Call<TypedPhase>& call) {
    // 类型检查后，call_ext 保证存在
    const auto& ext = call.ext;  // CallExt<TypedPhase>
    if (ext.arg_types.size() >= 2) {
        // 验证参数类型匹配
    }
}

// 解析阶段不会携带类型信息
void process_parsed(const Call<ParsedPhase>& call) {
    // call.ext 是 monostate，编译期优化掉
    static_assert(std::is_same_v<decltype(call.ext), std::monostate>);
}
```

---

## 4. ABF v2 二进制协议

### 4.1 Wire 格式

**关键挑战**：二进制格式必须支持**未知扩展**和**未来新增节点类型**，同时保持零拷贝和向后兼容。

每个节点的统一格式：

```
┌──────────────────────────────────────────┐
│  Tag: varint                             │ ← 节点类型
│  (0x01~0x7F 保留核心类型，0x80+ 扩展类型)  │
├──────────────────────────────────────────┤
│  ExtensionID: varint                     │ ← 来自 E::id
├──────────────────────────────────────────┤
│  ExtensionLength: varint                 │ ← 0 表示无扩展
├──────────────────────────────────────────┤
│  ExtensionPayload: N bytes               │ ← 反射序列化的扩展数据
├──────────────────────────────────────────┤
│  CorePayload: ...                        │ ← 节点核心负载
│  (子节点引用 + 字面量 + 元数据)           │
└──────────────────────────────────────────┘
```

**设计亮点**：
- **ExtensionID** 允许旧版本跳过未知扩展（向前兼容）
- **ExtensionLength** 支持零拷贝（直接用 `std::span` 切片扩展数据）
- **新增节点类型**只需分配新 Tag，无需修改旧格式
- **Tag 范围预留**：0x01~0x7F 为核心节点类型，0x80+ 为未来扩展节点类型

### 4.2 Tag 分配表

| Tag | 节点类型 | 说明 |
|-----|----------|------|
| 0x00 | (保留) | 空/无效节点 |
| 0x01 | LiteralInt | 整数字面量 |
| 0x02 | Variable | 变量引用 |
| 0x03 | Call | 函数调用 |
| 0x04 | IfExpr | 条件分支 |
| 0x05 | Lambda | 函数定义 |
| 0x06 | LetBinding | let 表达式 |
| 0x07 | Definition | define 定义 |
| 0x08~0x7F | (预留核心) | 未来核心节点类型 |
| 0x80+ | ExtendedNode | 扩展节点类型（由运行时注册） |

### 4.3 完整序列化实现

```cpp
// aura/binary/serializer.hpp
namespace aura::binary {

template <ast::Extension E>
class ExtensibleSerializer {
public:
    // 序列化一个 Expr 节点到 buffer
    uint32_t serialize_expr(const ast::Expr<E>& expr, Buffer& buf) {
        uint32_t node_start = buf.size();

        // 1. 写 Tag
        uint32_t tag = expr_to_tag(expr);
        append_varint(buf, tag);

        // 2. 写 Extension（利用反射自动处理）
        append_varint(buf, E::id);
        uint32_t ext_pos = buf.size();
        append_varint(buf, 0);  // 先占位 ExtensionLength

        size_t ext_write_start = buf.size();
        serialize_extension(expr, buf);  // 反射驱动
        uint32_t ext_len = buf.size() - ext_write_start;

        // 回填 ExtensionLength
        write_varint_at(buf, ext_pos, ext_len);

        // 3. 写 CorePayload（递归序列化子节点）
        serialize_payload(expr, buf);

        return node_start;
    }

private:
    // 反射驱动的 extension 序列化
    void serialize_extension(const ast::Expr<E>& expr, Buffer& buf) {
        std::visit([&](const auto& node) {
            if constexpr (std::is_same_v<typename decltype(node)::ext_type, std::monostate>) {
                // 无扩展数据，不写任何内容
            } else {
                serialize_reflectable(node.ext, buf);
            }
        }, static_cast<const std::variant<...>&>(expr));
    }

    // P2996 反射通用序列化
    template <typename T>
    void serialize_reflectable(const T& obj, Buffer& buf) {
        // 使用 C++26 P2996 std::meta 反射遍历所有非静态数据成员
        constexpr auto members = std::meta::nonstatic_data_members_of(^^T);
        template for (auto mem : members) {
            const auto& field = obj.*[:mem:];
            using FieldT = std::remove_cvref_t<decltype(field)>;

            if constexpr (std::is_arithmetic_v<FieldT>) {
                append_varint(buf, field);
            } else if constexpr (std::is_same_v<FieldT, SourceLocation>) {
                append_varint(buf, field.line);
                append_varint(buf, field.column);
            } else if constexpr (is_reflectable_v<FieldT>) {
                serialize_reflectable(field, buf);
            } else {
                // fallback：memcpy 原始字节
                append_raw(buf, &field, sizeof(FieldT));
            }
        }
    }

    uint32_t expr_to_tag(const ast::Expr<E>& expr) {
        return std::visit([](const auto& node) -> uint32_t {
            using NodeT = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<NodeT, ast::LiteralInt<E>>) return 0x01;
            if constexpr (std::is_same_v<NodeT, ast::Variable<E>>)   return 0x02;
            if constexpr (std::is_same_v<NodeT, ast::Call<E>>)       return 0x03;
            if constexpr (std::is_same_v<NodeT, ast::IfExpr<E>>)     return 0x04;
            if constexpr (std::is_same_v<NodeT, ast::Lambda<E>>)     return 0x05;
            return 0x80;  // ExtendedNode
        }, static_cast<const std::variant<...>&>(expr));
    }
};

} // namespace aura::binary
```

### 4.4 反序列化实现（零拷贝路径）

```cpp
// aura/binary/deserializer.hpp
namespace aura::binary {

template <ast::Extension E>
class ExtensibleDeserializer {
public:
    // 反序列化：从 Buffer 解析出 Expr 树
    ast::ExprPtr<E> deserialize(std::span<const std::byte> data) {
        auto reader = VarintReader(data);

        uint32_t tag = reader.read_varint();
        uint32_t ext_id = reader.read_varint();

        // Extension 兼容处理
        handle_extension(ext_id, reader);

        // 根据 Tag 反序列化核心负载
        return deserialize_payload(tag, reader);
    }

private:
    void handle_extension(uint32_t ext_id, VarintReader& reader) {
        uint32_t ext_len = reader.read_varint();

        if (ext_id != E::id) {
            // 未知 Extension：跳过（向前兼容）
            // 返回 span 指向扩展数据，可存入 ExtendedNode 供后续升级
            auto ext_data = reader.read_span(ext_len);
            last_unknown_ext = UnknownExtension{ext_id, ext_data};
        } else if (ext_len > 0) {
            // 已知 Extension：零拷贝切片
            auto ext_data = reader.read_span(ext_len);
            deserialize_known_extension(ext_data);
        }
        // ext_len == 0：无扩展，无需处理
    }

    ast::ExprPtr<E> deserialize_payload(uint32_t tag, VarintReader& reader) {
        switch (tag) {
        case 0x01: return deserialize_literal_int(reader);
        case 0x02: return deserialize_variable(reader);
        case 0x03: return deserialize_call(reader);
        case 0x04: return deserialize_if_expr(reader);
        case 0x05: return deserialize_lambda(reader);
        default:
            // 未知 Tag：保留 raw blob
            return make_extended_node(tag, reader.read_remaining());
        }
    }

    ast::ExprPtr<E> make_extended_node(uint32_t tag, std::span<const std::byte> data) {
        auto node = std::make_unique<ast::ExtendedNode<E>>();
        node->original_tag = tag;
        node->raw_data = data;  // 零拷贝引用原 Buffer
        return std::make_unique<ast::Expr<E>>(std::move(*node));
    }

private:
    std::optional<UnknownExtension> last_unknown_ext;
};

} // namespace aura::binary
```

---

## 5. Racket 端序列化器框架

Racket 端负责生成 ABF v2 二进制数据。Racket 不直接实现零拷贝（语言运行时不支持），但需要输出与 C++26 端匹配的二进制格式。

```racket
;; aura/private/abf-serialize.rkt
#lang racket

(provide serialize-expr serialize-delta)

;; ABF v2 格式常量
(define ABF-MAGIC #"ABF2")
(define TAG-LITERAL-INT  #x01)
(define TAG-VARIABLE     #x02)
(define TAG-CALL         #x03)
(define TAG-IF          #x04)
(define TAG-LAMBDA      #x05)

;; 序列化一个表达式为 ABF v2 二进制
(define (serialize-expr expr [phase-id 0])
  (define buf (make-bytes 1024 0))
  (define pos (reversible-writer buf))
  
  ;; Header
  (write-bytes ABF-MAGIC buf)
  (write-varint! pos 1)                    ;; version
  (write-varint! pos phase-id)             ;; 当前 Phase ExtensionID
  
  ;; 节点
  (let node-start (pos))
  (write-varint! pos (expr->tag expr))
  (write-varint! pos phase-id)             ;; ExtensionID
  (let ext-start (pos))
  (write-varint! pos 0)                    ;; 占位 ExtensionLength
  
  ;; 写扩展数据 (根据 phase-id)
  (write-extension! pos expr phase-id)
  (let ext-end (pos))
  (write-varint-at! ext-start (- ext-end ext-start))  ;; 回填长度
  
  ;; 写核心负载
  (write-core-payload! pos expr)
  
  (take-bytes buf (- pos 0)))

;; 序列化 Delta 更新: 只传输变更的子树
(define (serialize-delta expr base-version)
  (define delta-buf (make-bytes 512 0))
  (define pos (reversible-writer delta-buf))
  (write-bytes ABF-MAGIC delta-buf)
  (write-varint! pos 1)                    ;; version
  (write-varint! pos base-version)         ;; base version reference
  (write-varint! pos (expr-hash expr))     ;; new version ID
  
  ;; 只写变更节点 (通过比较 base-version 的 AST 决定)
  (write-changed-nodes! pos expr base-version)
  
  (take-bytes delta-buf (- pos 0)))
```

---

## 6. 在 Ghuloum 增量开发中逐步引入 Trees that Grow

| 增量步骤 | 新增内容 | Extension | 二进制变更 |
|----------|----------|-----------|-----------|
| Step 1-6 | 基础求值器 | ParsedPhase (monostate) | ABF v2 基础格式，无扩展数据 |
| Step 7 | 源位置跟踪 | LocatedPhase (SourceLocation) | 每个节点 +8 bytes 源位置扩展 |
| Step 8-10 | 静态类型 | TypedPhase (TypeInfo) | 类型检查后可选写类型元数据 |
| Step 11 | 宏展开 | ExpandedPhase (Scope/Binding) | 新增扩展 ID + 绑定表序列化 |
| Step 12+ | 优化标记 | OptimizedPhase | 新增扩展 ID + 优化元数据 |

**关键原则**：每一步新增的扩展数据通过 `ExtensionID` 隔离，旧版本（未升级到该阶段的解析器）会正确跳过未知扩展数据，保持向前兼容。

---

## 7. 总结

| 特性 | 实现方式 | 收益 |
|------|----------|------|
| AST 可生长 | template `<Extension E>` + Concepts | 类型安全地添加字段和节点 |
| 零拷贝反序列化 | ExtensionLength + `std::span` | 极高性能 |
| 向前/向后兼容 | ExtensionID + 长度前缀 | 老版本可读新格式 |
| 反射驱动 | P2996 `std::meta` | 新增节点几乎零工作量 |
| 节点级独立扩展 | 每个节点类型有自己的 XExtension | 精细控制不同阶段的扩展 |
| 无外部依赖 | 纯 C++26 | 完全控制 |
| Delta 增量增量 | 版本化变更子树传输 | 最小化通信量 |

---

> 相关文档：[ARCHITECTURE.md](./ARCHITECTURE.md) | [DESIGN.md](./DESIGN.md) | [ROADMAP.md](./ROADMAP.md)
