# Fixnum/Bool 立即值编码 — 消除值分发

## 现状

当前 `EvalValue` 是 `std::variant`（具体实现为 tagged union）：

```cpp
// value.ixx
using EvalValue = std::variant<
    std::monostate,     // void
    std::int64_t,       // int
    double,             // float
    std::uint64_t,      // pair index, string index, closure id, etc.
    bool,               // bool
    // ...
>;
```

每次访问值需要 `std::visit` 或手写 `switch(type)`: ~10-20ns 的 type dispatch overhead。

## 方案：Tagged Pointer 编码

用指针/整数的低 bit 编码类型，消除 variant dispatch：

```
  63              3  2  1  0
  ┌──────────────────┬──────┐
  │    value         │ tag  │
  └──────────────────┴──────┘

  tag=00  →  Pointer (pair/string/closure/hash/vector opaque)
  tag=01  →  Fixnum (63-bit signed int, >>1 to decode)
  tag=10  →  Bool/Void (value=0→#f, 1→#t, 2→void, ...)
  tag=11  →  Float (NaN-boxed double)
```

### Fixnum 编码/解码

```cpp
// 编码: n → (n << 1) | 1  （在寄存器里就是一条 shift+or）
inline EvalValue make_int(int64_t n) noexcept {
    return static_cast<uint64_t>((n << 1) | 1);
}
// 解码: (v >> 1)  （一条 arith-shift）
inline int64_t as_int(EvalValue v) noexcept {
    return static_cast<int64_t>(v) >> 1;
}
```

### Bool/Void 编码

```cpp
inline EvalValue make_bool(bool b) noexcept {
    return static_cast<uint64_t>(b ? 2 : 0);  // tag=10, value=0/1
}
inline bool is_bool(EvalValue v) noexcept {
    return (v & 3) == 2;
}
inline bool as_bool(EvalValue v) noexcept {
    return (v & 2) != 0;
}
inline EvalValue make_void() noexcept {
    return static_cast<uint64_t>(4);  // tag=10, value=1
}
```

### Pointer 类型

```cpp
// 原始指针存在低 3 位清零的地址空间里
template<typename T>
inline T* as_ptr(EvalValue v) noexcept {
    return reinterpret_cast<T*>(v & ~3ULL);
}
```

各类型通过高位地址范围区分（arena 分配保证）：

| 类型 | 地址范围 |
|------|---------|
| Pair | arena[0..N) |
| String | string_heap 指针 |
| Closure | closures_ 指针 |
| Hash | hash_heap 指针 |
| Vector | vector_heap 指针 |

### 比较操作优化

```cpp
// `=` 在 fixnum 上就是一条指令
inline bool eq_fixnum(EvalValue a, EvalValue b) noexcept {
    return a == b;  // 相同 tag+value → 相等
}

// `<` 需要先解码再比较
inline bool lt_fixnum(EvalValue a, EvalValue b) noexcept {
    return static_cast<int64_t>(a >> 1) < static_cast<int64_t>(b >> 1);
}
```

## 迁移策略

### Phase 1：Fixnum + Bool/Void 立即值

1. 修改 `value.ixx` 中的 `EvalValue` 定义：从 `std::variant` 改为 `uint64_t`
2. 改写 `make_int`, `as_int`, `is_int`, `make_bool`, `as_bool`, `is_bool`, `make_void`
3. 修改所有 `visit` 模式为 bit-tag  dispatch
4. 更新调试输出函数

### Phase 2：Pointer 类型适配

1. Pair/Closure/Hash/Vector/String 改为指针 + tag 编码
2. 更新所有 `as_pair_idx`, `make_pair` 等函数
3. Arena 分配器确保对齐到 8 bytes

## 改动文件

| 文件 | 改动 |
|------|------|
| `src/compiler/value.ixx` | `EvalValue` 改为 `uint64_t`, 重写所有 make/as/is 函数 |
| `src/compiler/value_impl.cpp` | `format_value`, `is_truthy` 等适配新编码 |
| `src/compiler/evaluator_impl.cpp` | 所有 `types::as_*` 调用点检查兼容性 |
| `src/compiler/ir_executor_impl.cpp` | IR interpreter 中的值操作适配 |
| `src/compiler/aura_jit.cpp` | LLVM IR 中的 tag dispatch 适配 |

## 收益

| 操作 | 当前 (variant) | 优化后 (tagged ptr) | 加速 |
|------|---------------|-------------------|------|
| `make_int(42)` | variant 构造 + 赋值 | `OR 1` 一条指令 | ~10x |
| `(+ a b)` | 2x variant visit + 1x 构造 | 3x int64 指令 | ~3x |
| `(< a b)` | 2x visit + 1x cmp | 2x shift + 1x cmp | ~3x |
| `(= a b)` | 2x visit + 1x cmp | `CMP` 一条指令 | ~5x |
| `(if cond ...)` | `is_truthy` visit | bit test | ~2x |
| 内存占用 | 24 bytes/variant | 8 bytes/uint64 | 3x |

## 风险

- `std::visit` 安全但慢；bit tag 需要手动维护类型安全
- Pointer 编码假设地址空间低 3 位清零（x86_64 arm64 都满足）
- Float NaN-boxing 兼容性（某些架构额外处理）
- 跨模块 ABI：EvalValue 大小变了，影响 FFI
