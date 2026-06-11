# Rule:Normalize — 代码规范系统

**Status**: Design + P0 Implemented (2026-05-25)

## 1. 问题

LLM 生成的代码风格不一致、anti-pattern 重复出现、项目规范变更需要批量迁移。当前 query + mutate 原语可以做一次性修复，但无法管理、持久化、批量应用规范规则。

## 2. 解决方案

规则 = 可命名的 (pattern, replacement) 对，附元数据。

```lisp
(rule:define 'simplify-boolean-if
  :pattern "(if ?cond #t #f)"
  :replace "?cond")
```

已有基础设施：
- `query:pattern` — 模式匹配
- `mutate:replace-value` / `mutate:rebind` — 值替换
- `mutate:wrap` / `mutate:insert-child` — 结构变换

## 3. EDSL API

```
(rule:define name :pattern expr :replace expr [:condition expr])
(rule:apply name) → count of applied fixes
(rule:apply-all) → total count
(rule:list) → registered rules
(rule:list-violations) → audit only, no changes
```

## 4. 实现

P0 用 Aura 实现（lib/std/rule.aura），组合现有 query/mutate 原语：

```lisp
(define rules '())

(define (rule:define name . args)
  (let ((pattern "") (replace "") (condition #t))
    ;; parse keywords
    (set! rules (cons (list name pattern replace condition)
                      (remove name rules)))
    name))

(define (rule:apply name)
  (let ((r (find-rule name)))
    (apply-rule r)))

(define (apply-rule r)
  (let* ((pattern (list-ref r 1))
         (replace (list-ref r 2))
         (matches (query:pattern pattern)))
    (if (pair? matches)
      (begin
        (for-each (lambda (node) ...) matches)
        (length matches))
      0)))
```

## 5. 场景

- 代码风格统一（if simplifications, naming conventions）
- Anti-pattern 自动修复（div-by-zero, null-car）
- 迁移/重构（API rename）
- Agent 行为规范（best practices）
