# Aura 语言快速入门

用 Aura 写代码。10 分钟。

## 运行方式

```bash
# 管道模式 — 每行一个表达式
echo '(+ 1 2)' | ./build/aura          # → 3

# 多行管道
printf '(+ 1 2)\n(* 3 4)\n' | ./build/aura

# 交互式 REPL
./build/aura
Aura v0.1
> (+ 1 2)
3

# 格式化代码
./build/aura --fmt myfile.aura

# CaaS 服务
printf '(+ 1 2)' | ./build/aura --serve
{"status":"ok","value":"3"}
```

## 基础表达式

```scheme
; 算术
(+ 1 2 3 4)              ; → 10  多参数
(* 2 (+ 3 4))            ; → 14  嵌套
(/ 10 3)                 ; → 3   整数除法
(/ 10.0 3)               ; → 3.33333  浮点
(modulo 7 3)             ; → 1
(quotient 7 3)           ; → 2

; 比较 — 返回 #t / #f
(= 5 5)                  ; → #t
(< 1 2)                  ; → #t
(> 3 1)                  ; → #t

; 布尔
(and #t #f)              ; → #f
(or #t #f)              ; → #t
(not #f)                ; → #t

; 字符串
(string-append "hello" " " "world")  ; → "hello world"
(string-length "hello")              ; → 5

; 类型谓词
(integer? 42)            ; → #t
(float? 3.14)            ; → #t
(string? "hi")           ; → #t
(boolean? #t)            ; → #t
(procedure? +)           ; → #t
```

## 变量和函数

```scheme
; define — 顶层绑定
(define x 42)
(define name "Aura")

; 函数定义
(define (square x) (* x x))
(square 5)               ; → 25

; lambda
(define double (lambda (x) (* x 2)))
(double 10)              ; → 20

; 立即调用 lambda
((lambda (x y) (+ x y)) 3 4)  ; → 7

; 局部绑定
(let ((x 10) (y 20))
  (+ x y))                ; → 30

; let* — 顺序绑定
(let* ((x 2) (y (* x 3)))
  (+ x y))                ; → 8

; letrec — 递归绑定（阶乘）
(letrec ((fact (lambda (n)
  (if (= n 0) 1 (* n (fact (- n 1)))))))
  (fact 5))               ; → 120

; 命名 let（尾递归）
(let loop ((n 5) (acc 1))
  (if (= n 0) acc (loop (- n 1) (* acc n))))  ; → 120
```

## 条件

```scheme
(if (< 2 3) "yes" "no")   ; → "yes"

; when / unless — 不需要 else 分支
(when (> 5 3) (display "greater"))
(unless (= 1 2) (display "not equal"))

; cond — 多分支
(cond
  ((< 5 3) "less")
  ((= 5 3) "equal")
  (else "greater"))        ; → "greater"

; case — 值匹配
(case 2
  ((1) "one")
  ((2) "two")
  (else "other"))          ; → "two"

; and / or 短路
(and #f (error "not reached"))   ; → #f
(or #t (error "not reached"))    ; → #t
```

## 列表

列表是 pair 链，空列表用 `'()` 表示。

```scheme
; 构造
(cons 1 (cons 2 (cons 3 '())))     ; → (1 2 3)
(list 1 2 3)                       ; → (1 2 3)
'(1 2 3)                           ; → (1 2 3)  引用形式

; 访问
(car (list 1 2 3))                 ; → 1
(cdr (list 1 2 3))                 ; → (2 3)
(list-ref (list 10 20 30) 1)      ; → 20

; 操作
(length (list 1 2 3))              ; → 3
(append (list 1 2) (list 3 4))     ; → (1 2 3 4)
(reverse (list 1 2 3))             ; → (3 2 1)
(take 3 (list 1 2 3 4 5))         ; → (1 2 3)
(drop 3 (list 1 2 3 4 5))         ; → (4 5)

; 标准库 — 需 require
(require std/list)
; 或 (import "std/list")  无前缀
(map (lambda (x) (* x 2)) (list 1 2 3))   ; → (2 4 6)
(filter (lambda (x) (> x 2)) (list 1 2 3 4 5))  ; → (3 4 5)
(foldl + 0 (list 1 2 3))           ; → 6
(range 1 6)                        ; → (1 2 3 4 5)
```

## 向量

```scheme
(vector 1 2 3)                          ; → <vector[0]>
(vector-ref (vector 10 20 30) 1)        ; → 20
(vector-length (vector 1 2 3))          ; → 3
(vector-set! (vector 1 2 3) 0 99)       ; 原地修改
(make-vector 3 42)                      ; → <vector[1]>  (3个42)
(vector->list (vector 1 2 3))           ; → (1 2 3)
(list->vector (list 1 2 3))             ; → <vector[2]>
```

## 哈希表

```scheme
; 构造
(hash "a" 1 "b" 2 "c" 3)

; 读写
(define h (hash))
(hash-set! h "name" "Aura")
(hash-ref h "name")                     ; → "Aura"
(hash-ref h "missing")                  ; 返回 void
(hash-length h)                         ; → 1
(hash-keys h)                           ; → ("name")
(hash-values h)                         ; → ("Aura")

; 标准库 — 函数式操作
(require std/hash)
(hash-set (hash "a" 1) "b" 2)          ; 返回新表（带 a b）
(hash->list (hash "a" 1 "b" 2))        ; → (("a" 1) ("b" 2))
(hash-merge (hash "a" 1) (hash "b" 2))
(alist->hash (list (list "a" 1)))       ; 从 alist 构建
```

## 变参函数和 apply

```scheme
; 变参 lambda — rest 参数收集剩余参数
((lambda (x . rest) (cons x (length rest))) 1 2 3)
; → (1 . 2)

; 纯 rest（没有固定参数）
; Aura 不支持 (lambda rest ...) 语法，用 dotted 形式：
((lambda (. rest) (length rest)) 1 2 3)  ; 不支持
((lambda (x . rest) (cons x rest)) 1 2 3) ; → (1 2 3)

; apply — 将列表展开为参数
(apply + (list 1 2 3))                ; → 6
(apply string-append (list "a" "b"))  ; → "ab"

; 变参函数定义
(define (sum . nums) (apply + nums))
(sum 1 2 3 4 5)                       ; → 15
```

## 字符操作

Aura 的字符用整数表示（ASCII 码点）。

```scheme
(char=? 65 65)              ; → #t   (65 = #\A)
(char<? 65 66)              ; → #t
(char-alphabetic? 65)       ; → #t   (A-Z, a-z)
(char-numeric? 48)          ; → #t   (48 = #\0)
(char-whitespace? 32)       ; → #t   (space, tab, newline)
(char-upcase 97)            ; → 65   (a → A)
(char-downcase 65)          ; → 97   (A → a)
(char->integer 65)          ; → 65   (identity)
(integer->char 65)          ; → 65   (identity)
```

## 字符串操作

```scheme
(string->list "ABC")                   ; → (65 66 67)
(list->string (list 65 66 67))         ; → "ABC"
(string-join (list "a" "b" "c") ", ")  ; → "a, b, c"
(string-copy "hello")                  ; → "hello"
(string-fill! (string-copy "xxx") 89)  ; 改为 "YYY"

; 从标准库
(require std/string)
(string-split "a,b,c" ",")             ; → ("a" "b" "c")
(string-trim "  hello  ")              ; → "hello"
(string-pad "5" 3 "0")                 ; → "005"
```

## 格式化输出

```scheme
; display — 打印不加引号
(display "hello")          ; 输出: hello
(display 42)               ; 输出: 42

; write — 打印加引号/转义
(write "hello")            ; 输出: "hello"

; newline — 换行
(newline)

; format — 格式化字符串 (SRFI-28)
(format "~a = ~a" "x" 42)        ; → "x = 42"
(format "~s = ~s" "x" 42)        ; → "\"x\" = 42"
(format "line1~%line2")           ; → "line1\nline2"
(format "hello ~~ world")         ; → "hello ~ world"
```

## 错误处理

```scheme
; try/catch 捕获错误
(try
  (/ 1 0)                        ; 除以零
  (catch (e) (display "oops")))

; raise 主动抛错
(try
  (raise "something wrong")
  (catch (e) (display e)))

; assert
(assert (= 1 1))                 ; → 1
(try
  (assert (= 1 2))              ; 返回 error
  (catch (e) "assertion failed"))
```

## 模块系统

```scheme
; require — 加载标准库（带前缀）
(require std/list)              ; 注入 list:map, list:foldl 等
(list:map (lambda (x) (* x 2)) (list 1 2 3))

; require + all: — 无前缀
(require std/list all:)
; 现在 map, foldl 可直接用

; import — 底层接口
(import "std/list")              ; 无前缀
(import "std/list" "my:")        ; 自定义前缀

; export — 模块声明公有接口
; lib/std/hash.aura:
(export hash-set hash-ref hash-length hash->list)
```

## 测试

```scheme
(require std/test all:)

(test-suite "my tests"
  (check= 4 (+ 2 2))
  (check (> 5 3))
  (check (string? "hello")))

(run-tests)
```

## CaaS 服务模式

```bash
# 启动 serve
printf '(+ 1 2)' | ./build/aura --serve
{"status":"ok","value":"3"}

# 定义函数
printf '{"cmd":"define","code":"(define (f x) (+ x 1))","name":"f"}'>
  | ./build/aura --serve
{"status":"ok"}

# 调用
printf '{"cmd":"exec","code":"(f 41)"}' | ./build/aura --serve
{"status":"ok","value":"42"}

# 模块管理
printf '{"cmd":"module","op":"compile","name":"demo","code":"(define (f x) (+ x 1))"}'
  | ./build/aura --serve
{"status":"ok"}
```

## 完整示例

```scheme
; 词频统计
(require std/csv)
(require std/hash)
(require std/list)

(define (word-frequency filename)
  (let ((lines (read-lines filename))
        (freq (hash)))
    (for-each (lambda (line)
      (for-each (lambda (word)
        (let ((count (hash-ref freq word)))
          (if (void? count)
              (hash-set! freq word 1)
              (hash-set! freq word (+ count 1)))))
        (string-split line " ")))
      lines)
    freq))

; CSV 解析
(define csv-data (csv-parse "name,age\nAlice,30\nBob,25"))
; → (("name" "age") ("Alice" "30") ("Bob" "25"))
```

## 文件 IO

```scheme
(read-file "data.txt")              ; 读整个文件 → 字符串
(write-file "out.txt" "content")    ; 写文件
(copy-file "src.txt" "dst.txt")     ; 复制
(delete-file "tmp.txt")             ; 删除
(file-exists? "data.txt")           ; → #t / #f
(file-size "data.txt")              ; → 字节数
(directory-list ".")                ; → ("file1" "dir2" ...)
(read-lines "data.txt")             ; → 行列表
```

## 宏

```scheme
(defmacro (twice x) (+ x x))
(twice 5)                           ; → 10

; 准引用模板
(defmacro (my-if cond then else)
  `(if ,cond ,then ,else))
```

---

**继续阅读**：`docs/roadmap.md`
