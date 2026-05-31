# 编译期常量求值 — 消除运行时构造

## 现状

```scheme
;; 每次文件 load 时执行:
(define *evo-store* (hash))        ;; 运行时调用 make_hash()
(define *evo-ttl* (hash))          ;; 运行时调用 make_hash()
(define *evo-type-ops* (hash))     ;; 运行时调用 make_hash()
(define *evo-eviction-policy* "noeviction")  ;; 运行时分配字符串

;; 耗时：每个 hash 构造 ~200ns，3 个 hash + 2 个 string ≈ 1μs
```

对于 JIT/AOT 编译的场景，这些常量完全可以在编译期计算。

## 方案

### 阶段 1：Define-Constant 检测

在 `needs_tree_walker_fallback` 或 lowering 阶段识别纯常量定义：

```cpp
// 模式: (define name (hash))          → 纯常量 hash
//       (define name "string")        → 纯常量 string
//       (define name (+ 1 2))         → 纯常量 int
//       (define name (list 1 2 3))    → 纯常量 list

struct ConstDef {
    std::string name;
    types::EvalValue value;  // 编译期求值的常量
};
```

在 `eval()` 函数的 lowering 阶段之前扫描顶级 define：

```cpp
// service.ixx eval() 中
for (auto& def : extract_const_defs(*flat_ptr, *pool_ptr, expanded_root)) {
    evaluator_.top_env().bind(def.name, def.value);
}
```

这样 `(define *evo-store* (hash))` 在编译期就创建好 hash 对象并绑定到环境中，**运行时零开销**。

### 阶段 2：编译期求值图

构建表达式的常量传播链：

```cpp
// 常量传播规则:
//   (hash)              → make_hash()     （编译期执行）
//   (hash) 的结果引用    → 编译期已知指针
//   (list "a" 1)        → 编译期创建 pair chain
//   (+ 1 2)             → compile-time eval → 3
//   (string-append "a" "b") → "ab" 编译期求值
```

实现：在 lowering → IR 之前，对 define 的 value 子树做常量折叠：

```cpp
EvalResult try_const_eval(FlatAST& flat, StringPool& pool, NodeId val_id) {
    auto v = flat.get(val_id);
    switch (v.tag) {
        case LiteralInt: return make_int(v.int_value);
        case LiteralString: return make_string(pool.resolve(v.sym_id));
        case Call: {
            auto callee = pool.resolve(flat.get(v.child(0)).sym_id);
            if (callee == "hash") return make_hash();
            if (callee == "list") {
                // 创建 pair chain
                EvalValue result = make_void();
                for (int i = v.children.size() - 1; i >= 1; --i) {
                    auto item = try_const_eval(flat, pool, v.child(i));
                    result = cons(item, result);
                }
                return result;
            }
            // 无法求值 → 返回 nullopt
            return std::nullopt;
        }
        default: return std::nullopt;
    }
}
```

### 阶段 3：AOT 嵌入常量数据

在 `--emit-binary` 路径中，将编译期常量序列化到二进制中：

```
AOT binary layout:
  .text:  (编译后的函数代码)
  .rodata:
    hash_t *evo_store = { .bucket_count=16, .entries = {...} }  ← 编译期预填充
    hash_t *evo_ttl = { .bucket_count=16, .entries = {} }
    string_pool[] = { "hello", "world", ... }
```

这样 AOT 二进制启动时不需要构造任何 evo-kv 基础设施——直接从 `.rodata` 加载。

## 改动文件

| 文件 | 改动 |
|------|------|
| `src/compiler/service.ixx` | `eval()` 中增加 `extract_const_defs` 前置 pass |
| `src/compiler/evaluator_impl.cpp` | `try_const_eval()` 实现 |
| `src/compiler/lowering_impl.cpp` | 常量折叠扩展 |
| `src/main.cpp` | `--emit-binary` 增加常量数据段嵌入式 |

## 收益

| 场景 | 当前 | 优化后 | 加速 |
|------|------|--------|------|
| 冷启动 evo-kv（5 个 define） | ~1μs 构造 | 0 (编译期完成) | ∞ |
| `(hash)` 每次 define | ~200ns | 0 | ∞ |
| 常量字符串定义 | ~100ns + heap alloc | 0 (rodata) | ∞ |
| AOT 二进制加载 | runtime 构造 | 直接映射 .rodata | ~100x |

## 风险

- 纯常量检测必须保守——不能误判有副作用的表达式（如 `(hash-from-list ...)`）
- 编译期 `make_hash()` 创建的 hash 对象必须与运行时兼容（内存布局、GC 标记）
- AOT 序列化需要处理指针 relocations（hash 内部的桶指针）
