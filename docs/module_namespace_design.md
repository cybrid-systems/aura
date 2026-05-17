# Aura 命名空间 + 模块系统设计 v2

> 从裸 `import` 到带前缀的命名空间模块。

---

## 现状

```
(require std/list)     → 展开为 (import "std/list.aura")
(import "path.aura")   → eval 文件内容到全局 env

问题:
  1. 两个文件都 define 同名函数 → 后者覆盖前者
  2. 没有模块隔离
  3. 不知道函数来自哪个模块
```

---

## 设计

### 1. 模块即环境 (Module = Env)

```
每个 .aura 文件在加载时创建独立的 Env。
模块对外暴露其 define 的符号。
加载语法:
  (import mod-name)           → 把 define 注入当前 env（当前行为）
  (import mod-name prefix:)   → 加前缀注入
  (use mod-name)              → 返回模块对象（不注入全局）
```

### 2. 带前缀导入

```scheme
; 无前缀（当前行为，简洁但有冲突风险）
(import "math.aura")
(pi)  → 3.14159

; 带前缀（推荐方式）
(import "math.aura" math:)
(math:pi)  → 3.14159
(math:sqrt 16)  → 4

; use = 不注入，直接返回模块对象
(let ((json (use "json.aura")))
  (json:parse "[1,2,3]"))
```

### 3. 实现: 前缀重命名

```
import 不是原语，是语法变换:

  (import "fmt.aura" fmt:) 
  → 等价于:
    (let ((__mod__ (load-module "fmt.aura")))
      (begin
        (define fmt:println (module-get __mod__ 'println))
        (define fmt:printf  (module-get __mod__ 'printf))
        ...))

load-module 返回模块环境对象（Env 的 Aura 包装）。
module-get 从环境对象中按名称查找绑定。
```

### 4. 模块对象

```scheme
; 模块对象是一个<module>值（新类型）
; 支持:
(module? obj)                     → #t
(module-export mod 'function-name)  → 获取模块导出
(module-keys mod)                 → 列出所有导出名

; 模块对象也可以直接当函数调用（语法糖）:
(mod 'function-name)  → (module-get mod 'function-name)
```

### 5. 导出声明 (显式 API)

```scheme
; 默认: 所有 define 都可导出
; 显式 export 声明 API:
(export square cube)

(define (square x) (* x x))
(define (cube x) (* x x x))
(define (internal-helper x) ...)  ; 不导出
```

未 export 的函数对其他模块不可见。

### 6. 模块缓存

```
load-module 按路径缓存:
  1. 检查 modules_ 缓存表
  2. 如果已加载 → 返回缓存的模块对象
  3. 如果未加载 → parse + eval + 缓存

缓存按文件路径的 resolved absolute path 做 key。
```

### 7. 标准库模块化

```
当前: 所有 define 注入全局 env
改为:
  (require std/list)    → 展开为 (import "std/list.aura" std:)  
  → 使用时: (std:map f lst)
  (require std/math)    → (std:pi) (std:sqrt ...)
  (require std/json)    → (std:json-parse ...)

向后兼容: (require std/list) 加 :all 后缀 
  (require std/list all:)  → 无前缀注入（当前行为）
```

### 8. 循环依赖检测

```
load-module 在加载时记录当前加载栈。
如果遇到已在栈中的模块 → 循环依赖错误。

(define (load-module path)
  (if (in-load-stack? path)
      (error "circular dependency:" path)
      (begin
        (push-load-stack! path)
        (let ((env (eval-file path)))
          (pop-load-stack!)
          env))))
```

### 9. 迁移路径

```
Phase 1: module-get / module-keys 原语 + <module> 值类型  ← 现在
Phase 2: import 语法变换支持前缀
Phase 3: export 声明
Phase 4: 标准库改用前缀
Phase 5: 循环依赖检测
```
