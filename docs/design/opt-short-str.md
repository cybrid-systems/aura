# 短字符串内联 — 消除 string_heap 索引

## 现状

```cpp
// 字符串分配：push_back + 返回 index
auto sidx = string_heap_.size();   // 取 index
string_heap_.push_back(str);       // 拷贝到 heap
return make_string(sidx);          // 返回 index 编码的 EvalValue

// 字符串访问：index → heap 取回
auto sidx = as_string_idx(val);
auto& s = string_heap_[sidx];      // heap 查找
```

每次字符串创建/读取都要 heap push_back + index 查找。对于 evo-kv 的场景（`"k"`, `"v"`, `"hello"`），**大部分字符串 ≤ 7 bytes**。

## 方案：NaN-boxed Short String

在 Fixnum/Float tagged pointer 方案之上（见 `opt-tagged-ptr.md`），利用 NaN-boxing 的空闲 bit：

对于 IEEE 754 double NaN 值：
```
 63 (sign) 62..52 (exp)  51..0 (mantissa)
  x        11111111111      xxxxxxxxxxxxx...
```

当 exp = 0x7FF（全 1）且 mantissa ≠ 0 时为 NaN。可以用 mantissa 的 49 bits 编码数据。

短字符串编码：

```
  63  62..52           51              48..0
  ┌──┬──────────┬────────────────────────────┐
  │ 1 │1111111111│ 0 │  len(3)  │ 字符数据    │
  └──┴──────────┴───┴──────────┴─────────────┘
    ↑ NaN sign    ↑string-tag  ↑3-bit length  ↑最多 6 chars in remaining 45 bits (7 ASCII chars * 6 bits? 或 5 chars * 9 bits?)

  或者更简单: 直接复用 Fixnum tag (tag=01):
  63      8   7   6   5   4   3  2  1  0
  ┌──────────────────────────┬──┬────────┬──┐
  │     字符数据 (6 chars)    │len│ str-t │01│
  └──────────────────────────┴──┴────────┴──┘
                                   ↑tag=01 (fixnum space)
  
  len=000~111 (0~7 bytes)
  str-t=1 → short string
  str-t=0 → fixnum
```

### 编码/解码

```cpp
// 最多 6 字节字符串直接内联在 fixnum tag 空间
static constexpr std::size_t SHORT_STR_MAX = 6;

inline bool is_short_str(EvalValue v) noexcept {
    return (v & 0x8F) == 0x09;  // tag=01, str-t=1
    //    vvvv 低位: 0000 1001
    //                        ^str-t=1
    //                      ^^^tag=01
}

inline EvalValue make_short_str(const char* s, uint8_t len) noexcept {
    uint64_t v = 0x09;  // tag=01 + str-t=1
    for (int8_t i = len - 1; i >= 0; --i)
        v = (v << 8) | static_cast<uint8_t>(s[i]);
    v |= static_cast<uint64_t>(len) << 3;  // 低 3 位存 tag
    return v;
}

inline std::string_view as_short_str(EvalValue v) noexcept {
    static char buf[SHORT_STR_MAX + 1];
    auto len = (v >> 3) & 0x7;
    for (uint8_t i = 0; i < len; ++i)
        buf[i] = (v >> (8 * i + 6)) & 0xFF;  // 跳过 tag+str-t byte
    buf[len] = '\0';
    return {buf, len};
}
```

### 集成到现有字符串系统

在 `make_string` 中自动选择 short/inline：

```cpp
inline EvalValue make_string(const std::string& s) noexcept {
    if (s.size() <= SHORT_STR_MAX)
        return make_short_str(s.data(), s.size());
    // 长字符串：走现有 heap 路径
    auto si = string_heap_.size();
    string_heap_.push_back(s);
    return make_long_str(si);
}
```

`equal?` 比较时先检查 tag：

```cpp
bool string_eq(EvalValue a, EvalValue b) noexcept {
    if (a == b) return true;  // 相同指针/相同短字符串
    if (is_short_str(a) && is_short_str(b))
        return (a & ~0x07ULL) == (b & ~0x07ULL);  // 忽略 tag 位比较数据
    // 长字符串：走 heap 比较
    auto& sa = string_heap_[as_string_idx(a)];
    auto& sb = string_heap_[as_string_idx(b)];
    return sa == sb;
}
```

## 收益分析

| 场景 | 当前 | 优化后 | 加速 |
|------|------|--------|------|
| `"k"` 创建 | heap push + index | 寄存器内 3 条指令 | ~20x |
| `"hello"` 创建 | heap push + index | 寄存器内 6 条指令 | ~10x |
| `equal? "k" "k"` | 2x heap lookup + strcmp | 1x uint64 cmp | ~10x |
| evo-kv key 操作（~10 byte keys） | 50% 命中 short str | — | ~5x |
| evo-kv val 操作（~100 byte vals） | 0% 命中 | — | 0x |

## 改动文件

| 文件 | 改动 |
|------|------|
| `src/compiler/value.ixx` | `make_string`, `is_string`, `as_string` 增加 short str 路径 |
| `src/compiler/value_impl.cpp` | `format_value` 适配 short str |
| `src/compiler/ir_executor_impl.cpp` | IR 中的 string 指令适配 |
| `src/compiler/evaluator_impl.cpp` | eval_flat string 操作检查 short str |

## 风险

- 最大 6 字节——长于 6 的字符串仍走 heap，split-type 增加复杂度
- `as_string` 返回 `std::string_view` 时需要区分 short/long（short 用 stack buf）
- 与 Fixnum tag 共享 tag=01namespace，Fixnum 可表示范围缩小到 61-bit
