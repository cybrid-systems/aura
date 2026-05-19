# Aura 代码格式化设计

> 极简 S-表达式格式化，零配置。

---

## 现状

```
没有格式化工具。
写出来的代码缩进全靠手敲或 LLM 生成的习惯。
```

---

## 设计

### 1. 目标

```
1. 可预测的缩进
2. 80 列软限制（不强硬换行）
3. 一致性（同一段代码永远格式化成一样）
4. 零配置
```

### 2. 核心策略

```
;; 短表达式一行:
(define (add x y) (+ x y))

;; 长表达式换行 + 缩进 2 格:
(define (fib n)
  (if (< n 2)
      n
      (+ (fib (- n 1))
         (fib (- n 2)))))

;; 多参数换行对齐:
(let ((x 10)
      (y 20)
      (z 30))
  (+ x y z))

;; define-struct:
(define-struct point (x y z))

;; lambda:
(map (lambda (x)
       (* x x))
     '(1 2 3 4 5))
```

### 3. 格式化规则

```
1. 根规则: 一个 S-表达式 = 一行或者多行
   - 一行: (fn arg arg arg)  ← 总长度 < 80
   - 多行: 参数各自换行，缩进 2 格

2. 特殊形式缩进:
   define / define-struct / lambda / let / letrec / let*:
     体缩进 2 格
     if: cond 跟 if 同行，then/else 换行缩进 2 格
     cond: 每个 clause 缩进 2 格
     match: 每个 pattern 缩进 2 格

3. 函数调用:
   - 短参数: 一行 (f a b c)
   - 长参数: 换行，每个参数一行
     (f a
        b
        c)

4. 空列表: () 始终不换行

5. 注释:
   ; 行注释保持原位
   ;; 块注释前空一行
```

### 4. 实现策略

```
1. 读取 FlatAST → 递归节点
2. 对每个节点判断: "是否应该换行?"
   - 叶子节点 (LiteralInt, Variable): 不换行
   - 复合节点: 如果序列化后长度 < 80 → 一行
   - 否则 → 换行

3. 特殊形式的缩进规则用 meta 表驱动:
   (define name body)  → body 缩进 2
   (if cond then else) → then/else 缩进 2
   (lambda (args) body) → body 缩进 2
   (let ((b v) ...) body) → bindings + body 缩进 2
```

### 5. CLI 集成

```bash
./aura --fmt file.aura        # 格式化文件，输出到 stdout
./aura --fmt -i file.aura     # 原地格式化
./aura --fmt --check file.aura  # 只检查格式是否正确（CI 用）

# 示例:
$ cat test.aura
(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))

$ ./aura --fmt test.aura
(define (fib n)
  (if (< n 2)
      n
      (+ (fib (- n 1))
         (fib (- n 2)))))
```

### 6. 实现简案

```
format_node(flat, pool, node_id, indent, column):
  v = flat.get(node_id)
  
  # 叶子节点
  if is_leaf(v): return format_leaf(v, pool)
  
  # 复合节点: 先尝试一行
  one_line = try_format_one_line(flat, pool, node_id)
  if len(one_line) + column < 80:
    return one_line
  
  # 换行
  return format_multiline(flat, pool, node_id, indent)
```

### 7. 迁移路径

```
Phase 1: format_node 递归格式化单个表达式  ← 现在
Phase 2: --fmt CLI 命令
Phase 3: 特殊形式缩进规则
Phase 4: 行注释保留
Phase 5: --check CI 模式
```
