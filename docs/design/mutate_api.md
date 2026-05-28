# 结构化变异 API — Issue #11

## 重构原语

### `mutate:rename-symbol`

```scheme
(mutate:rename-symbol old-name new-name [summary]) → #t/#f
```

AST 中所有同名 symbol 的 def + use 节点。支持 Variable、Define、DefineType、DefineModule、Let、LetRec、Set、MacroDef。

```scheme
(set-code "(define (square x) (* x x))")
(eval-current)
(mutate:rename-symbol "square" "double") → #t
(double 5) → 25
```

### `mutate:extract-function`

```scheme
(mutate:extract-function node-id new-name [summary]) → (define-id . call-id)
```

将子树提取为新的顶层函数定义。自动分析自由变量（过滤 builtin），生成带参数的 lambda，原地替换为函数调用。新定义插入 workspace root 首位。

```scheme
;; 提取 (* x 3) → mul3
(set-code "(define (calc x) (+ (* x 3) 1))")
(eval-current)
(mutate:extract-function <mul-call-id> "mul3") → (define-id . call-id)
(eval-current)
(calc 2) → 7    ; 原始函数不变
(mul3 5) → 15   ; 新函数: (define (mul3 x) (* x 3))
```

### `mutate:inline-call`

```scheme
(mutate:inline-call call-node-id [summary]) → #t/#f
```

将函数调用原地内联：克隆函数体，将形式参数替换为实际参数。
支持命名函数和内联 lambda。被内联函数的定义必须在当前 AST 中。

```scheme
(set-code "(begin (define (f x) (+ x 1)) (define (g x) (f x)))")
(eval-current)
(mutate:inline-call <(f x) call-id>) → #t
(eval-current)
(g 5) → 6  ; g 的 body 现在直接是 (+ x 1)
```

### `mutate:move-node`

```scheme
(mutate:move-node node-id new-parent-id new-position [summary]) → #t/#f
```

将子树从当前位置移到新的父节点下指定位置。
自动做循环检测。支持跨父节点移动。

```scheme
(set-code "(begin (display 1) (display 2) (display 3))")
(eval-current)
(mutate:move-node <display(3)-id> <root-id> 0) → #t
(eval-current)  ; 输出 312
```

### `mutate:replace-pattern`

```scheme
(mutate:replace-pattern pattern-string replacement-string [summary]) → #t/#f
```

模式匹配替换：查找所有匹配模式的节点，替换为替换模板。
`...` 作为通配符，匹配任意单个子树并在替换中代入。

```scheme
;; 精确替换
(mutate:replace-pattern "(* 2 x)" "(+ x x)")

;; 通配符替换: 将任意除法包裹守卫
(mutate:replace-pattern "(/ ... ...)" "(if (= ?2 0) 0 (/ ?1 ?2))")
;; 注: ?1, ?2 为占位符，后续版本支持
```

## 语义保持

每个原语在 mutate 后验证：
- `typecheck-current` 通过
- `eval-current` 结果与 mutate 前一致
- 已有 19 个 suite 测试覆盖

## 实现细节

- `FlatAST` 新增 `add_raw_node()` 和 `parent_of()` 公共 API
- 修复 `add_lambda` / `add_call` 漏掉的 `link_children` 调用
- 修复 `set_child` / `insert_child` / `remove_child` 的 `parent_` 维护
- 修复 Begin 评估器中 NULL_NODE children 的容错
