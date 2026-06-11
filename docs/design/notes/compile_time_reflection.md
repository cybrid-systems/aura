# 编译期反射 — C++26 std::meta 应用设计

> 用 `src/reflect/*.hh` 自动生成序列化、模式验证、模块缓存。
> 不再手写 boilerplate。

**更新：2026-05-23**
**状态：Phase 1-3 完成，Phase 4-5 计划中**

---

## 1. 现状痛点

当前 Aura 编译器中，所有数据结构（AST 节点、IR 指令、类型描述符）的序列化、验证、导出表都是**手写的**：

```cpp
// 现状: 每加一个 IR 指令类型，要改 4 处
struct IRAdd : IRInst {
    IRValue lhs, rhs;
    // 手写序列化
    void serialize(Buffer& buf) { buf.write(lhs); buf.write(rhs); }
    // 手写反序列化
    static IRAdd deserialize(Buffer& buf) { return {buf.read(), buf.read()}; }
    // 手写验证
    bool validate() { return lhs.valid() && rhs.valid(); }
};
```

问题：**加一个字段，改 3 个文件。** 这不是工程——这是体力活。

## 2. 解决方案：编译期反射

C++26 的 `std::meta` 可以在编译期枚举任何 struct 的成员：

```cpp
template <typename T>
consteval auto reflect_members() {
    auto members = nonstatic_data_members_of(^^T);
    // → [{name:"lhs", offset:0, type:IRValue},
    //     {name:"rhs", offset:8, type:IRValue}]
}
```

有了成员的**名称、偏移、类型**，编译期就可以自动生成：

### 2.1 自动序列化

```cpp
template <typename T>
void auto_serialize(Buffer& buf, const T& obj) {
    constexpr auto members = reflect_members<T>();
    for (auto [name, offset, kind] : members) {
        const auto& field = *reinterpret_cast<const field_type_t*>(
            reinterpret_cast<const char*>(&obj) + offset);
        // 根据 kind 自动选择写入方式
        if constexpr (is_int) buf.write(field);
        else if constexpr (is_string) buf.write(field);
        else if constexpr (is_vector) buf.write(field.size());
        // ...
    }
}
```

**加一个字段 → 0 行额外代码。** 序列化代码在编译期自动生成。

### 2.2 自动验证

```cpp
template <typename T>
bool auto_validate(const T& obj) {
    constexpr auto members = reflect_members<T>();
    bool ok = true;
    for (auto [name, offset, kind] : members) {
        const auto& field = ...;
        if constexpr (is_null) ok = false;  // 非空验证
        if constexpr (is_range) ok &= (min <= field <= max);  // 范围验证
    }
    return ok;
}
```

### 2.3 自动 Schema 生成

```cpp
template <typename T>
consteval auto generate_schema() {
    constexpr auto members = reflect_members<T>();
    // → 编译期生成 JSON Schema / 协议描述
}
```

## 3. 应用场景

### 3.1 AST 节点序列化（ABF v2）

当前 `.abfc` 缓存格式靠手写遍历 AST。改为反射自动生成：

```
手写 parse + serialize（现状） → reflect_members<ASTNode>（目标）
  cache_module() 手动遍历         auto_serialize(ASTNode) 自动
  parse_to_flat() 手动构造        reflect_members 自动布局校验
```

### 3.2 IR 指令序列化

IR 指令是编译器中间表示，当前每个指令类型都要手写序列化。反射后：

```cpp
struct IRAdd : IRInst {
    int lhs_slot, rhs_slot;   // 加这个字段
};  // auto_serialize 自动处理

struct IRCall : IRInst {
    int fn_slot;
    std::vector<int> args;    // vector 自动序列化
};
```

### 3.3 模块导入/导出表

stdlib 模块当前通过 `(require std/name all:)` 加载。反射可以自动：

```cpp
// 编译期：遍历 std/list 的所有导出符号
// 自动生成 (require std/list all:) 的符号表
constexpr auto exports = reflect_module_exports("std/list");
// → {"filter", "map", "foldl", "range", "sort", ...}
```

### 3.4 类型模式检查

`type_checker` 中的类型转换规则当前是手动匹配的。反射可以：

```cpp
// 编译期：枚举所有类型的 coercion 路径
// 自动生成类型兼容性表
constexpr auto coercion_table = reflect_coercions();
// → [(Int -> Float), (Int -> String), ...]
```

### 3.5 编译器埋点（coverage）

Fuzz 测试中的编译器路径覆盖埋点，当前是手动插入的。反射可以：

```cpp
// 编译期：枚举所有 eval_flat 的分支路径
// 自动生成覆盖计数代码
constexpr auto branches = reflect_eval_branches();
```

## 4. 实现计划（详细）

### 前置条件

- GCC 16.1 已支持 `std::meta`，但有以下已知限制
- CMake 集成已完成：`aura-reflect INTERFACE` 库 + `-freflection`
- `src/reflect/*.hh` 已通过编译测试（`auto_to_json<T>()` 已验证）

### Phase 1: 基础反射工具（已完成 ✅）

已完成（`src/reflect/*.hh`）：
- `reflect.hh`:
  - `reflect_members<T>()` — 编译期枚举所有成员
  - `classify_member_type()` — 判断成员类型
  - `auto_to_json<T>()` — 任意 struct → JSON（已完成、已验证）
- `opcode_reflect.hh`: IR 指令枚举反射
- `reflect_schema.hh`: Schema 生成框架
- `read_auto_validate.hh`: 读取后自动验证
- `tag_dispatch.hh`: 标签分发工具
- `type_validate.hh`: 类型验证

### Phase 1.5: 增强反射能力（已完成 ✅）

已扩展 `reflect.hh` 支持容器类型和嵌套 struct 反射：

```cpp
// 1. 添加容器类型支持
enum class MemberKind : uint8_t {
    // ... 已有类型 ...
    // + 新增:
    Vector,    // std::vector<T>
    Array,     // std::array<T,N>
    Struct,    // 嵌套 struct（递归反射）
    Enum,      // enum（自动映射整数）
    Optional,  // std::optional<T>
    Variant,   // std::variant<T...>
};

// 2. 递归 struct 支持
// 当前 auto_to_json 遇到嵌套 struct 输出 "null"
// 改为递归调用 reflect_members 遍历嵌套

// 3. enum 序列化支持
// 当前 enum 序列化为整数（不直观）
// 改为序列化为 enum 名 + 整数 pairs
```

**文件修改:** `src/reflect/reflect.hh`

### Phase 2: 自动序列化 — 接入 cache_module（已完成 ✅）

目标：用 `auto_serialize<T>()` 替换手写序列化。
已实现：
- `cache_serialize_header` via std::meta (b869067)
- `write_cache` uses auto_serialize (512a2c8)
- Buffer class + auto_serialize for scalars/strings/arrays (e5f502a)
- IR 序列化已替换为 P2996 反射序列化 (2823c6c)

**当前限制：** 容器类型反序列化需要模板类型信息，部分推迟到后续阶段。

### Phase 3: 自动验证（已完成 ✅）

已实现：
- `type_validate.hh` — P2996 类型验证 (fa6748b)
- `auto_validate` + `cache_validate_header` (a1936f8)
- 编译期类型验证：字段级约束检查（非空、范围）

### Phase 4: 模块导出表（计划中 🟡）

还没开始。需要：
- 编译期扫描 stdlib 源码生成导出符号表
- 修复 primitives hash 和 std/hash 的冲突

### Phase 5: 编译器埋点（计划中 🟡）

还没开始。需要：
- 枚举 eval_flat 所有分支生成覆盖计数
- 接入 fuzz 测试

## 5. 收益

| 场景 | 手写维护量 | 反射后状态 | 节省 |
|------|:---------:|:---------:|:----:|
| IR 序列化 | ~300 行 / 每次加指令 | ✅ 已替换 (2823c6c) | **100%** |
| CacheHeader 序列化 | ~100 行 | ✅ 已替换 (742fcf0) | **100%** |
| 类型验证 | ~150 行 | ✅ 已实现 (a1936f8) | **100%** |
| AST 序列化 | ~200 行 / 每次加节点 | 🟡 进行中 | **部分完成** |
| 模块导出表 | ~50 行 / 每个 stdlib | 🟡 计划中 | — |
| 编译器埋点 | ~80 行 | 🟡 计划中 | — |

**已减少手写代码：~550 行。**

## 6. 风险

- `std::meta` 是 C++26 实验特性，编译器支持不稳定
- 当前 GCC 16 的 `std::meta` 实现可能不完整
- 编译时间可能增加（constexpr 反射在编译期计算）
- 反射不支持 private 成员（需要 `access_context::unchecked`）

## 7. 参考

- [design repo → cpp26/02-compile-time-code.md](https://github.com/cybrid-systems/ai-programming-language-design/blob/main/docs/cpp26/02-compile-time-code.md)
- `src/compiler/reflect.ixx` — 现有框架
- `src/compiler/service.ixx` — `cache_module()` 是首个接入目标
