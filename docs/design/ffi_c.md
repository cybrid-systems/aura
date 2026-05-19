# C FFI 设计

## 用户 API

```scheme
;; 加载动态库
(define m (c-load "libmylib.so" "cdecl"))

;; 声明外部函数
(define add (c-func m "add" Int Int Int))
(add 3 4)  ;; → 7

;; 处理字符串
(define greet (c-func m "greet" String String))
(greet "Aura")  ;; → "Hello, Aura!"

;; 指针操作（不透明类型）
(define create (c-func m "create_buffer" Opaque Int))
(define process (c-func m "process_buffer" Int Opaque))
(let ((buf (create 1024)))
  (process buf))
```

## 类型映射

| C 类型 | Aura 类型 | 编码 |
|--------|----------|------|
| int32_t | Int | int64_t (truncate) |
| int64_t | Int | int64_t |
| double | Float | double |
| char | Char (Int) | int64_t |
| void | Void | 0 |
| char* | String | string_heap idx |
| void* | Opaque | int64_t ptr |
| struct | Raw | bytes (future) |

## 实现架构

```
                    Aura 代码
                       │
              (c-func "add" Int Int Int)
                       │
             CFFI::lookup("add") → fn_ptr
                       │
              marshall(args) → C call → marshall(result)
                       │
                    EvalValue
```

### 三层实现

**层 1: 运行时桥接 (C++)**

```cpp
// cffi.h — C FFI bridge
struct ForeignFunc {
    void* fn_ptr;           // dlsym result
    std::string lib_name;
    std::vector<int> arg_types;  // 0=Int, 1=Float, 2=String, 3=Opaque
    int ret_type;
};

// Evaluator 扩展
std::unordered_map<std::string, ForeignFunc> foreign_funcs_;
std::unordered_map<std::string, void*> loaded_libs_;

// 调用时:
EvalValue call_foreign(ForeignFunc& ff, const std::vector<EvalValue>& args) {
    // marshall args → C values
    // call function pointer
    // marshall result → EvalValue
}
```

**层 2: JIT 集成**

JIT 编译的函数可以通过 ORC JIT 符号表直接调用 C 函数（零开销）：

```cpp
// 在 JIT 中注册 DLL 符号
for (auto& [name, ptr] : loaded_symbols) {
    jit_->register_symbol(name, ptr);
}
```

编译后的 Aura 代码调用 `c-func` 返回的外部函数时，JIT 代码直接：

```asm
mov rdi, arg1
mov rsi, arg2
call [add_fn_ptr]
mov [result], rax
```

**层 3: dlopen 管理**

```cpp
void* load_library(const std::string& path) {
    auto h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    return h;
}

void* lookup_symbol(void* lib, const std::string& name) {
    return dlsym(lib, name.c_str());
}
```

## 执行路径

| 场景 | 路径 | 开销 |
|------|------|------|
| eval-flat 树遍历 | C++ bridge → marshal → call → marshal | ~50ns |
| IR interpreter | C++ bridge → marshal → call → marshal | ~50ns |
| LLVM JIT (-O2) | 直接 call 指令 | ~1ns |
| JIT + inline | LLVM 自动内联 C 函数 | ~0ns |

## 安全性

1. **沙箱默认关闭** — `c-load` 只能在非 `--safe` 模式下使用
2. **类型验证** — `c-func` 声明时的类型签名在调用时检查
3. **Opaque 指针** — 不透明指针类型，不能解引用，只能传给其他 FFI 函数
4. **字符串安全** — 传入 C 时复制到临时 `\0` 结尾缓冲区；返回时从 `\0` 截断的字符串复制回 string_heap

## 实现步骤

| 步骤 | 内容 | 时间 |
|------|------|------|
| 1 | `c-load` + `c-func` 原语 (dlopen + dlsym) | 1h |
| 2 | 类型 marshalling (Int/Float/String/Opaque/void) | 1h |
| 3 | `c-func` 返回可调用的 ClosureRef → 树遍历桥接 | 1h |
| 4 | JIT 符号注册 → 零开销调用 | 1h |
| 5 | 结构化类型 (struct 按字段访问) | 2h (future) |

## 示例: 调用 C 标准库

```scheme
;; qsort
(define m (c-load ""))  ;; 已加载的符号
(define compare (c-func m "compare" Int Opaque Opaque))

;; 直接的 C 函数
(define sqrt (c-func m "sqrt" Float Float))
(sqrt 9.0)  ;; → 3.0

;; 通过主程序符号
(define rand (c-func m "rand" Int))
(rand)  ;; → 12345
```

## 与 LLVM JIT 的协同

JIT 已经注册了 libc 符号 (printf, fprintf, fputc)。FFI 扩展这个机制：

1. `c-load` → `dlopen` → 枚举所有符号 → 注册到 JIT 符号表
2. `c-func` → 返回一个 "外部函数" 值（ClosureRef 的变体）
3. 树遍历路径：通过 C++ 桥接调用
4. JIT 路径：直接符号解析 + call 指令
