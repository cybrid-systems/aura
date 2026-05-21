# 编译期反射 — C++26 std::meta 应用设计

> 用 `reflect.ixx` 自动生成序列化、模式验证、模块缓存。
> 不再手写 boilerplate。

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

## 4. 实现计划

### Phase 1: 基础反射工具（已完成框架）

- [x] `reflect.ixx` — `reflect_members<T>()`, `classify_member_type()`
- [ ] 添加 `MemberKind::Vector`, `MemberKind::String`, `MemberKind::Struct`
- [ ] 添加嵌套 struct 的递归反射支持
- [ ] 添加 enum 序列化支持

### Phase 2: 自动序列化（~2-3h）

- [ ] `auto_serialize<T>(buf, obj)` — 任意 struct → bytes
- [ ] `auto_deserialize<T>(buf)` → bytes → struct
- [ ] 接入 `cache_module()`，替换当前手写序列化
- [ ] 验证 round-trip（序列化 → 反序列化 → 相等）

### Phase 3: 自动验证（~1h）

- [ ] `auto_validate<T>(obj)` — 字段级约束检查
- [ ] 接入 `validate_ast()`，替换手写验证
- [ ] `check-preconditions` EDSL 原语接入反射

### Phase 4: 模块导出表（~2h）

- [ ] `reflect_module_exports(name)` — 编译期扫描 stdlib 源码
- [ ] 自动生成 `(require std/name all:)` 的符号映射
- [ ] 减少手写错误（当前 std/hash 和 primitives 的 hash 函数冲突）

## 5. 收益

| 场景 | 手写维护量 | 反射后 | 节省 |
|------|:---------:|:-----:|:----:|
| AST 序列化 | ~200 行 / 每次加节点 | 0 | **100%** |
| IR 序列化 | ~300 行 / 每次加指令 | 0 | **100%** |
| 模块导出表 | ~50 行 / 每个 stdlib | 0 | **100%** |
| 类型验证 | ~150 行 | 0 | **100%** |
| 编译器埋点 | ~80 行 | 0 | **100%** |

**总手写代码减少：~780 行 → 0。**

## 6. 风险

- `std::meta` 是 C++26 实验特性，编译器支持不稳定
- 当前 GCC 16 的 `std::meta` 实现可能不完整
- 编译时间可能增加（constexpr 反射在编译期计算）
- 反射不支持 private 成员（需要 `access_context::unchecked`）

## 7. 参考

- [design repo → cpp26/02-compile-time-code.md](https://github.com/cybrid-systems/ai-programming-language-design/blob/main/docs/cpp26/02-compile-time-code.md)
- `src/compiler/reflect.ixx` — 现有框架
- `src/compiler/service.ixx` — `cache_module()` 是首个接入目标
