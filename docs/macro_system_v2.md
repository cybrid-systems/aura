# Aura Macro System v2: 设计

## 现状

当前 `defmacro` 是简单的模板替换（AST 子树替换），能力极其有限：
- 不支持代码模板（`quasiquote`）
- 不能生成新标识符
- 只做一遍展开（不递归）
- 无卫生性保证

## 设计目标

1. **可用的 DSL 构建能力** — 能实现 `define-struct`、`match` 等在 Aura 自身中
2. **简单实现** — 保持 Aura 的最小核心哲学
3. **AI 友好** — 展开结果可预测、可审计

## 核心设计：parser 级 quasiquote

`quasiquote`（`` ` ``）在 parser 层面展开为 `list`/`cons`/`quote` 调用链。
这是最重要的单个特性，因为 `defmacro` 缺的就是可读的代码模板。

### 展开规则

```
`expr   → (quote expr)                            ; 非列表
`(a b)  → (list (quote a) (quote b))               ; 列表，无 unquote
`(a ,b) → (list (quote a) b)                       ; unquote 插入值
`(a ,@b) → (cons (quote a) b)                     ; unquote-splicing 拼接
`(a ,@b c) → (cons (quote a) (append b (list (quote c))))
```

具体递归展开规则（标准 Scheme quasiquote）：

```
(quasiquote X) → (expand-qq X 0)

expand-qq(X, D) =
  if X is a pair:
    if X is (unquote Y) and D == 0: Y
    if X is (unquote Y) and D > 0:  (list 'unquote (expand-qq Y (- D 1)))
    if X is (unquote-splicing Y) and D == 0: error
    if X is (quasiquote Y):         (list 'quasiquote (expand-qq Y (+ D 1)))
    else (expand-qq-pair X D)
  else: (quote X)

expand-qq-pair((car . cdr), D) =
  if car is (unquote Y) and D == 0:
    if cdr is (unquote-splicing Z) and D == 0: (append Y Z)
    else (cons Y (expand-qq cdr D))
  if car is (unquote-splicing Y) and D == 0:
    (append Y (expand-qq cdr D))
  else:
    (cons (expand-qq car D) (expand-qq cdr D))
```

### Parser 实现

在 lexer 中：
- `` ` ``（backtick）→ `TokenKind::QuasiQuote`
- `,`（comma）→ `TokenKind::Unquote`
- `,@`（comma+at）→ `TokenKind::UnquoteSplicing`

在 parser 中新增 `parse_quasiquote()`：
- 读取下一个表达式
- 调用 `expand_qq(expr, 0)` 递归展开为 list/cons/quote/append 的 AST

```cpp
NodeId expand_qq(NodeId expr, int depth, FlatAST& flat, StringPool& pool);
```

### 示例展开

```
`(lambda (x) (+ x 1))
→ (list (quote lambda) (list (quote x)) (list (quote +) x (quote 1)))
```

```
`(define (,name ,@fields) (vector ',name ,@fields))
→ 假设 name = make-point, fields = (x y)
→ (list (quote define) (cons name fields) (list (quote vector) (cons (list 'quote name) fields)))
→ 运行时: (define (make-point x y) (vector 'make-point x y))
```

## 新增原语

### `gensym`

```
(gensym) → G__42     ; 每次返回不重复的符号
(gensym "prefix") → prefix__43
```

在 evaluator 中实现为单调递增计数器 + string pool。

### `symbol-append`

```
(symbol-append 'make- 'point) → make-point
(symbol-append "make-" 'point) → make-point  ; 接受 string 或 symbol
```

在 evaluator 中实现：concatenate 名字后 intern 到 string pool。

## 递归宏展开

当前 `macro_expand_all` 做最多 10 遍展开。但存在两个问题：
1. 展开结果中的内部 define 绑定的宏不会被递归展开
2. 展开后的代码中的 quasiquote 不会被递归展开

修复：展开循环中，每次展开后重新扫描所有节点，对新出现的宏调用继续展开。

## 卫生性 (v1 暂缓)

完整的卫生宏需要 `syntax-rules` 模式 + 重命名。v1 不实现。

最低限度的防意外捕获机制：
- `SyntaxMarker::MacroIntroduced` 已存在（标记宏引入的节点）
- v1 只保证：宏引入的变量不覆盖用户定义（但用户变量可能被宏的变量捕获）

## 汇总：实现路线

### Phase 1 (1-2 天)
1. Lexer: 添加 `` ` ``, `,`, `,@` token
2. Parser: `expand_qq` 递归展开
3. Test: quasiquote 基本展开正确

### Phase 2 (半天)
4. `gensym` 原语
5. `symbol-append` 原语

### Phase 3 (半天)
6. 递归宏展开（多遍）
7. 测试: `define-struct` 作为 defmacro + quasiquote 实现

### Phase 4 (可选)
8. documentation + examples
