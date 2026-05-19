# C FFI — 设计文档

**状态**: ✅ 已实现 (`5b8d496`)

## 用户 API

```scheme
(define lib (c-load "libm.so.6"))
(define sqrt (c-func lib "sqrt" 2 2))
(sqrt 9.0)  ;; → 3.0

(define strlen (c-func lib "strlen" 1 3))
(strlen "hello")  ;; → 5

(define malloc (c-func lib "malloc" 4 1))
(define free (c-func lib "free" 0 4))
(let ((p (malloc 1024))) (free p))
```

## 类型映射

| 标签 | Aura 类型 | C 类型 | 编码规则 |
|------|----------|--------|---------|
| 1 | Int | int64_t | `make_int` / `as_int` |
| 2 | Float | double | `make_float` / `as_float` |
| 3 | String | const char* | string_heap → null-terminated copy |
| 4 | Opaque | void* | `int64_t` 透传 |
| 0 | Void | void | 返回 0 |

## 实现架构

```
(c-load path)       → dlopen(path, RTLD_NOW) → lib index
(c-func lib name ret args...) → dlsym(lib, name) → FFIFunc{fn_ptr, name, ret, arg_types}
(apply FFI closure) → apply_closure() → type dispatch → call fn_ptr → marshal result
```

### 三层

**层 1: C++ Runtime Bridge** (已实现)
```cpp
// evaluator_impl.cpp
struct FFIFunc { void* fn_ptr; std::string name; int ret_type; std::vector<int> arg_types; };
static std::vector<FFIFunc> g_ffi_funcs;
static std::vector<void*> g_ffi_libs;

// apply_closure dispatch:
//   if closure_id has high bit set → look up FFIFunc
//   → marshal args (int64_t/double/char*/void*)
//   → call through reinterpret_cast fn ptr
//   → marshal result → EvalValue
```

**层 2: JIT Symbol Registration** (API 已实现，集成待做)

```cpp
// aura_jit.h
class AuraJIT {
    void register_symbol(const char* name, void* ptr);
};
```

`c-func` 可以调用 `jit_.register_symbol()` 将符号注册到 ORC JIT 符号表。
JIT 编译的代码可以直接 `call [symbol]` 而不是走 C++ bridge（零开销）。

**层 3: eval_flat 集成** (已实现)

树遍历求值器中所有闭包调用路径都包含 FFI 检测：

```cpp
if (cid & (1ULL << 63)) {  // 高位置位 = 外部函数
    return apply_closure(cid, args);
}
```

## 执行路径

| 场景 | 路径 | 开销 |
|------|------|------|
| 树遍历调用 FFI | `eval_flat` → `apply_closure` → marshal → C call | ~50ns |
| IR interpreter | `Call` handler → `apply_closure` → marshal → C call | ~50ns |
| LLVM JIT (future) | 直接 `call [fn]` 指令 | ~1ns |

## 示例

```scheme
;; libm
(define lib (c-load "libm.so.6"))
(define sqrt (c-func lib "sqrt" 2 2))   (sqrt 9.0)    ;; 3.0
(define pow (c-func lib "pow" 2 2 2))   (pow 2.0 10)  ;; 1024.0

;; libc string
(define strlen (c-func lib "strlen" 1 3))  (strlen "hello")  ;; 5
(define strcmp (c-func lib "strcmp" 1 3 3)) (strcmp "a" "b")  ;; -1
(define atoi (c-func lib "atoi" 1 3))       (atoi "42")       ;; 42

;; memory
(define malloc (c-func lib "malloc" 4 1))
(define free (c-func lib "free" 0 4))
(define memset (c-func lib "memset" 4 4 1 1))

(let ((buf (malloc 64)))
  (memset buf 0 64)
  (free buf))
```

## 安全性

- `--safe` 模式: 计划禁用 `c-load`
- Opaque 指针: 不能解引用，只能传递给其他 FFI 函数
- String: 从 Aura 传入 C 时复制到临时缓冲区（安全性 + 0-terminated）
- 类型检查: `c-func` 声明的类型签名在 marshal 时验证（数字）

## 扩展

新增类型只需:
1. 在 marshalling switch 中加一个 case
2. 定义 Aura ↔ C 的转换规则
