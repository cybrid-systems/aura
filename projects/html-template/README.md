# HTML Template Engine (Aura)

Aura 写的简易 HTML 模板引擎。演示用途。

## 使用

```bash
export AURA_PATH="../../lib"
echo '(include "templates" (hash "name" "World"))' | ../../build/aura
```

## 如何工作

```scheme
(import "std/json")
(import "std/string")

; 插值: 将 {{key}} 替换为 data 中的值
(define (interpolate template data)
  (define (replace s)
    ; 查找 {{...}} 模式
    ...)
  ...)

; 渲染: 加载模板 + 插值
(define (render template-name data)
  ...)
```
